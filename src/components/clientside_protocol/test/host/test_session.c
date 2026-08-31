/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* Host functional test of the session logic (without ESP-IDF/WebSocket).
 * Plays through the full path: hello -> hello_ack -> signed ota_check ->
 * ota_none -> verified command -> command_result. Build: test/host/README.md */
#include "fp_session.h"
#include "fp_auth.h"
#include "fountain_msgs.h"
#include "fountain_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t fp_now_ms(void) { return 1718370040000LL; }   /* fixed stub for the test */

#define CAPSZ 4096
static char g_last[CAPSZ];
/* ctx = target capture buffer (CAPSZ); NULL -> legacy global buffer. */
static bool cap_send(void *ctx, const char *json) {
    snprintf(ctx ? (char *)ctx : g_last, CAPSZ, "%s", json);
    return true;
}

static void snap(cJSON *dp, const cJSON *names, void *u) {
    (void)names; (void)u;
    cJSON_AddNumberToObject(dp, "Fon_Current_Pressure", 3.2);
    cJSON_AddNumberToObject(dp, "Fon_Current_State", 3);
}
static void oncmd(const cJSON *m, cJSON *res, void *u) {
    (void)m; (void)u;
    cJSON_AddStringToObject(res, "status", "applied");
    cJSON *rb = cJSON_AddObjectToObject(res, "readback");
    cJSON_AddNumberToObject(rb, "Fon_Current_State", 3);
}
/* Pressure-history stub: 3 samples in total, delivers from since_seq+1. */
static void fillhist(cJSON *body, uint32_t since, uint32_t maxs, void *u) {
    (void)u;
    cJSON_AddNumberToObject(body, "next_seq", 4);
    cJSON_AddNumberToObject(body, "first_seq_available", 1);
    cJSON *arr = cJSON_AddArrayToObject(body, "samples");
    for (uint32_t seq = since + 1; seq <= 3; seq++) {
        if (cJSON_GetArraySize(arr) >= (int)maxs) break;
        cJSON *row = cJSON_CreateArray();
        cJSON_AddItemToArray(row, cJSON_CreateNumber(seq));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(seq * 1000));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(2500));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(1));
        cJSON_AddItemToArray(arr, row);
    }
}

static void onwr(const cJSON *dp, cJSON *res, void *u) {
    (void)dp; (void)u;
    cJSON_AddStringToObject(res, "status", "applied");
}

static const char *KEYHEX =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const char *DID = "esp32-a1b2c3d4e5f6";
static const char *SNONCE = "9f3aK2pL0xQ7sV4nB1dC8g==";

static const char *type_of(const char *json) {
    static char buf[32];
    cJSON *m = cJSON_Parse(json);
    const cJSON *t = cJSON_GetObjectItem(m, "type");
    snprintf(buf, sizeof buf, "%s", cJSON_IsString(t) ? t->valuestring : "?");
    cJSON_Delete(m);
    return buf;
}
static const char *last_type(void) { return type_of(g_last); }

static const char *status_of(const char *json) {
    static char buf[32];
    cJSON *m = cJSON_Parse(json);
    const cJSON *st = cJSON_GetObjectItem(m, "status");
    snprintf(buf, sizeof buf, "%s", cJSON_IsString(st) ? st->valuestring : "?");
    cJSON_Delete(m);
    return buf;
}

/* Signed s2c command (server role) against the given session nonces/seq. */
static char *build_signed_cmd(const uint8_t *key, const char *snonce,
                              const char *cnonce, long seq, const char *msg_id) {
    cJSON *cmd = cJSON_CreateObject();
    cJSON_AddNumberToObject(cmd, "v", 2);
    cJSON_AddStringToObject(cmd, "type", "command");
    cJSON_AddNumberToObject(cmd, "ts", 1718370040000.0);
    cJSON_AddStringToObject(cmd, "msg_id", msg_id);
    cJSON_AddStringToObject(cmd, "command", "set_state");
    cJSON_AddStringToObject(cmd, "target_state", "Auto");
    fp_auth_message_sign(cmd, key, 32, "1", seq, "s2c", DID, snonce, cnonce);
    char *json = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    return json;
}

/* Two concurrent instances: independent nonces/sequences/replay windows —
 * the isolation proof for the local-server sessions (firmware_server plan
 * AP2). Returns the number of failed checks. */
static int two_instance_test(const fp_config_t *cfg, const uint8_t *key) {
    static const char *SN_A = "AAAAtestnonceA==";
    static const char *SN_B = "BBBBtestnonceB==";
    static char bufA[CAPSZ], bufB[CAPSZ];
    int fail = 0;

    fp_session_t a, b;
    fp_session_init(&a, cfg, cap_send, bufA);
    fp_session_init(&b, cfg, cap_send, bufB);
    fp_session_reset(&a);
    fp_session_reset(&b);

    fp_session_hello_send(&a);
    fp_session_hello_send(&b);
    printf("6) two hellos: A=%s B=%s, nonces differ=%d\n",
           type_of(bufA), type_of(bufB),
           strcmp(a.client_nonce, b.client_nonce) != 0);
    if (strcmp(type_of(bufA), "hello") || strcmp(type_of(bufB), "hello") ||
        strcmp(a.client_nonce, b.client_nonce) == 0) fail++;

    char ack[512];
    const char *fmt =
        "{\"v\":1,\"type\":\"hello_ack\",\"ts\":1,\"in_reply_to\":\"hello-1\","
        "\"accepted\":true,\"supported_protocols\":[1,2],\"server_ts\":1,"
        "\"auth_required\":true,\"auth_scheme\":\"hmac-sha256\","
        "\"auth_scope\":\"control\",\"auth_kid\":\"1\",\"server_nonce\":\"%s\"}";
    snprintf(ack, sizeof ack, fmt, SN_A);
    fp_session_message_handle(&a, ack);
    snprintf(ack, sizeof ack, fmt, SN_B);
    fp_session_message_handle(&b, ack);
    printf("7) both negotiated (A=%d B=%d), independent c2s seq (A=%ld B=%ld)\n",
           fp_session_negotiated(&a), fp_session_negotiated(&b),
           a.c2s_seq, b.c2s_seq);
    if (!fp_session_negotiated(&a) || !fp_session_negotiated(&b) ||
        a.c2s_seq != 1 || b.c2s_seq != 1) fail++;

    fp_session_message_handle(&a,
        "{\"v\":2,\"type\":\"ota_none\",\"ts\":1,\"in_reply_to\":\"chk-1\"}");
    fp_session_message_handle(&b,
        "{\"v\":2,\"type\":\"ota_none\",\"ts\":1,\"in_reply_to\":\"chk-1\"}");
    if (!fp_session_running(&a) || !fp_session_running(&b)) fail++;

    /* Same seq=1 on BOTH sessions must be accepted — separate replay windows. */
    char *cmdA = build_signed_cmd(key, SN_A, a.client_nonce, 1, "cmdA-1");
    char *cmdB = build_signed_cmd(key, SN_B, b.client_nonce, 1, "cmdB-1");
    fp_session_message_handle(&a, cmdA);
    fp_session_message_handle(&b, cmdB);
    printf("8) seq=1 on both sessions: A=%s B=%s (both applied?)\n",
           status_of(bufA), status_of(bufB));
    if (strcmp(status_of(bufA), "applied") || strcmp(status_of(bufB), "applied"))
        fail++;
    free(cmdB);

    /* Cross replay: the frame signed for A's nonces replayed into B must
     * fail the MAC (different nonces -> different context vector). */
    char *cmdA2 = build_signed_cmd(key, SN_A, a.client_nonce, 2, "cmdA-2");
    fp_session_message_handle(&b, cmdA2);
    printf("9) A-frame into B -> %s (expected rejected)\n", status_of(bufB));
    if (strcmp(status_of(bufB), "rejected") != 0) fail++;
    /* ...and the same frame is still valid for A (seq 2 fresh there). */
    fp_session_message_handle(&a, cmdA2);
    if (strcmp(status_of(bufA), "applied") != 0) fail++;
    free(cmdA);
    free(cmdA2);

    fp_session_close(&a);
    int wiped = a.server_nonce[0] == 0 && a.client_nonce[0] == 0 &&
                a.kid[0] == 0 && !a.negotiated && !a.running && a.c2s_seq == 0;
    printf("10) close zeroization A: wiped=%d, B untouched running=%d\n",
           wiped, fp_session_running(&b));
    if (!wiped || !fp_session_running(&b)) fail++;

    return fail;
}

int main(void) {
    uint8_t key[32];
    fp_auth_hex_decode(KEYHEX, key, sizeof key);

    fp_config_t cfg = {0};
    cfg.device_id = DID; cfg.serial = "000001C0C01FA82A";
    cfg.auth_kid = "1"; cfg.auth_key = key; cfg.auth_key_len = 32;
    cfg.fw_version = "2.0.0"; cfg.hw_rev = "rev-c";
    cfg.fill_snapshot = snap; cfg.on_command = oncmd; cfg.on_dp_write = onwr;
    cfg.fill_history_batch = fillhist;

    fp_session_t s;
    fp_session_init(&s, &cfg, cap_send, NULL);
    fp_session_reset(&s);

    int fail = 0;

    fp_session_hello_send(&s);
    printf("1) hello sent, type=%s\n", last_type());
    if (strcmp(last_type(), "hello") != 0) fail++;

    char ack[512];
    snprintf(ack, sizeof ack,
             "{\"v\":1,\"type\":\"hello_ack\",\"ts\":1,\"in_reply_to\":\"hello-1\","
             "\"accepted\":true,\"supported_protocols\":[1,2],\"server_ts\":1,"
             "\"auth_required\":true,\"auth_scheme\":\"hmac-sha256\","
             "\"auth_scope\":\"control\",\"auth_kid\":\"1\",\"server_nonce\":\"%s\"}",
             SNONCE);
    fp_session_message_handle(&s, ack);
    printf("2) after hello_ack -> sent type=%s (expected ota_check)\n", last_type());
    cJSON *oc = cJSON_Parse(g_last);
    int oc_signed = cJSON_HasObjectItem(oc, "auth");
    cJSON_Delete(oc);
    if (strcmp(last_type(), "ota_check") != 0 || !oc_signed) fail++;
    printf("   ota_check signed=%d, negotiated=%d\n", oc_signed,
           fp_session_negotiated(&s));

    fp_session_message_handle(&s,
        "{\"v\":2,\"type\":\"ota_none\",\"ts\":1,\"in_reply_to\":\"chk-1\"}");
    printf("3) ota_none -> running=%d\n", fp_session_running(&s));
    if (!fp_session_running(&s)) fail++;

    /* The "server" sends a signed command (uses the client_nonce generated by
     * the client + the server_nonce set in the ack). */
    cJSON *cmd = cJSON_CreateObject();
    cJSON_AddNumberToObject(cmd, "v", 2);
    cJSON_AddStringToObject(cmd, "type", "command");
    cJSON_AddNumberToObject(cmd, "ts", 1718370040000.0);
    cJSON_AddStringToObject(cmd, "msg_id", "cmd-1");
    cJSON_AddStringToObject(cmd, "command", "set_state");
    cJSON_AddStringToObject(cmd, "target_state", "Auto");
    fp_auth_message_sign(cmd, key, 32, "1", 1, "s2c", DID, SNONCE, s.client_nonce);
    char *cmdjson = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    fp_session_message_handle(&s, cmdjson);
    free(cmdjson);

    cJSON *cr = cJSON_Parse(g_last);
    const cJSON *st = cJSON_GetObjectItem(cr, "status");
    const char *status = cJSON_IsString(st) ? st->valuestring : "?";
    printf("4) command verified -> command_result status=%s\n", status);
    if (strcmp(last_type(), "command_result") != 0 || strcmp(status, "applied") != 0)
        fail++;
    cJSON_Delete(cr);

    /* Replay of the same command (same seq) must be rejected. */
    cJSON *cmd2 = cJSON_CreateObject();
    cJSON_AddNumberToObject(cmd2, "v", 2);
    cJSON_AddStringToObject(cmd2, "type", "command");
    cJSON_AddNumberToObject(cmd2, "ts", 1718370040000.0);
    cJSON_AddStringToObject(cmd2, "msg_id", "cmd-1");
    cJSON_AddStringToObject(cmd2, "command", "set_state");
    cJSON_AddStringToObject(cmd2, "target_state", "Auto");
    fp_auth_message_sign(cmd2, key, 32, "1", 1, "s2c", DID, SNONCE, s.client_nonce);
    char *c2 = cJSON_PrintUnformatted(cmd2);
    cJSON_Delete(cmd2);
    fp_session_message_handle(&s, c2);
    free(c2);
    cJSON *cr2 = cJSON_Parse(g_last);
    const char *st2 = cJSON_GetObjectItem(cr2, "status")->valuestring;
    printf("5) replay (seq=1 again) -> status=%s (expected rejected)\n", st2);
    if (strcmp(st2, "rejected") != 0) fail++;
    cJSON_Delete(cr2);

    /* Wire-value golden check: must match fountain_proto/_messages.py
     * (HISTORY_READ=22 / HISTORY_BATCH=23). */
    printf("5b) wire values: history_read=%d history_batch=%d\n",
           (int)FP_MSG_HISTORY_READ, (int)FP_MSG_HISTORY_BATCH);
    if (FP_MSG_HISTORY_READ != 22 || FP_MSG_HISTORY_BATCH != 23) fail++;

    /* Signed history_read -> history_batch with in_reply_to + samples. */
    cJSON *hr = cJSON_CreateObject();
    cJSON_AddNumberToObject(hr, "v", 2);
    cJSON_AddStringToObject(hr, "type", "history_read");
    cJSON_AddNumberToObject(hr, "ts", 1718370040000.0);
    cJSON_AddStringToObject(hr, "msg_id", "hist-1");
    cJSON_AddNumberToObject(hr, "since_seq", 1);
    cJSON_AddNumberToObject(hr, "max_samples", 100);
    fp_auth_message_sign(hr, key, 32, "1", 2, "s2c", DID, SNONCE, s.client_nonce);
    char *hrjson = cJSON_PrintUnformatted(hr);
    cJSON_Delete(hr);
    fp_session_message_handle(&s, hrjson);
    free(hrjson);

    cJSON *hb = cJSON_Parse(g_last);
    const cJSON *hbSamples = cJSON_GetObjectItem(hb, "samples");
    const cJSON *hbReply = cJSON_GetObjectItem(hb, "in_reply_to");
    int nSamples = cJSON_IsArray(hbSamples) ? cJSON_GetArraySize(hbSamples) : -1;
    printf("5c) history_read verified -> %s, samples=%d, in_reply_to=%s\n",
           last_type(), nSamples,
           cJSON_IsString(hbReply) ? hbReply->valuestring : "?");
    if (strcmp(last_type(), "history_batch") != 0 || nSamples != 2 ||
        !cJSON_IsString(hbReply) || strcmp(hbReply->valuestring, "hist-1") != 0)
        fail++;
    cJSON_Delete(hb);

    /* Unsigned history_read is discarded (no reply). */
    g_last[0] = '\0';
    fp_session_message_handle(&s,
        "{\"v\":2,\"type\":\"history_read\",\"ts\":1,\"msg_id\":\"hist-2\","
        "\"since_seq\":0}");
    printf("5d) unsigned history_read -> reply? %s (expected: none)\n",
           g_last[0] ? last_type() : "none");
    if (g_last[0] != '\0') fail++;

    fail += two_instance_test(&cfg, key);

    printf("%s\n", fail == 0 ? "SESSION TEST OK" : "SESSION TEST FAILED");
    return fail ? 1 : 0;
}
