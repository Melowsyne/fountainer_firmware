/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "fp_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fp_envelope.h"
#include "fp_log.h"
#include "fountain_msgs.h"

#if defined(ESP_PLATFORM)
#include "esp_random.h"
static void rand16(uint8_t *b) { esp_fill_random(b, 16); }
#else
static void rand16(uint8_t *b) { for (int i = 0; i < 16; i++) b[i] = (uint8_t)rand(); }
#endif

static const char *TAG = "fp_session";

/* ----- Helpers ----------------------------------------------------------- */
static const char *str_of(const cJSON *m, const char *k) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(m, k);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

/* Sends the finished message (takes over freeing it). */
static bool send_msg(fp_session_t *s, cJSON *msg) {
    if (!msg) return false;
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json) return false;
    bool ok = s->send(s->send_ctx, json);
    free(json);
    return ok;
}

/* Signs an s2c/c2s message (for the c2s session proof). */
static bool sign_c2s(fp_session_t *s, cJSON *msg) {
    return fp_auth_message_sign(msg, s->cfg->auth_key, s->cfg->auth_key_len,
                                s->kid, ++s->c2s_seq, "c2s",
                                s->cfg->device_id, s->server_nonce,
                                s->client_nonce);
}

/* Verifies an incoming s2c control message (MAC + anti-replay). */
static bool verify_s2c(fp_session_t *s, const cJSON *msg) {
    const char *reason = "?";
    if (!fp_auth_message_verify(msg, s->cfg->auth_key, s->cfg->auth_key_len,
                                s->kid, "s2c", s->cfg->device_id,
                                s->server_nonce, s->client_nonce, &reason)) {
        FP_LOGW(TAG, "auth verify failed: %s", reason);
        return false;
    }
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(msg, "auth");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(a, "seq");
    if (!fp_replay_check(&s->replay_s2c, (long)cJSON_GetNumberValue(seq))) {
        FP_LOGW(TAG, "seq replay detected");
        return false;
    }
    return true;
}

/* ----- Lifecycle ----------------------------------------------------- */
void fp_session_init(fp_session_t *s, const fp_config_t *cfg,
                     fp_transport_send_t send, void *send_ctx) {
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
    s->send = send;
    s->send_ctx = send_ctx;
}

void fp_session_reset(fp_session_t *s) {
    s->server_nonce[0] = s->client_nonce[0] = s->kid[0] = '\0';
    fp_replay_reset(&s->replay_s2c);
    s->c2s_seq = 0;
    s->report_seq = 0;
    s->negotiated = false;
    s->running = false;
}

void fp_session_close(fp_session_t *s) {
    /* End of a connection (local server sessions come and go): wipe the
     * session-scoped auth material completely — nonces/kid must not leak
     * into a later connection on the same slot (firmware_server.md §28). */
    memset(s->server_nonce, 0, sizeof s->server_nonce);
    memset(s->client_nonce, 0, sizeof s->client_nonce);
    memset(s->kid, 0, sizeof s->kid);
    fp_replay_reset(&s->replay_s2c);
    s->c2s_seq = 0;
    s->report_seq = 0;
    s->negotiated = false;
    s->running = false;
}

/* ----- hello / ota_check ------------------------------------------------ */
bool fp_session_hello_send(fp_session_t *s) {
    uint8_t nb[16];
    rand16(nb);
    char *b64 = fp_auth_base64_encode(nb, sizeof nb);
    if (!b64) return false;
    snprintf(s->client_nonce, sizeof s->client_nonce, "%s", b64);
    free(b64);

    fp_hello_t h = {0};
    h.device_id = s->cfg->device_id;          h.has_device_id = true;
    h.protocol_version = FP_WIRE_VERSION;      h.has_protocol_version = true;
    h.fw_version = s->cfg->fw_version;          h.has_fw_version = true;
    if (s->cfg->hw_rev) { h.hw_rev = s->cfg->hw_rev; h.has_hw_rev = true; }
    h.boot_reason = "power_on";                h.has_boot_reason = true;

    cJSON *sch = cJSON_CreateArray();
    cJSON_AddItemToArray(sch, cJSON_CreateString(FP_AUTH_SCHEME));
    h.auth_schemes = sch; h.has_auth_schemes = true;
    cJSON *kids = cJSON_CreateArray();
    cJSON_AddItemToArray(kids, cJSON_CreateString(s->cfg->auth_kid));
    h.auth_kids = kids; h.has_auth_kids = true;
    h.client_nonce = s->client_nonce; h.has_client_nonce = true;

    cJSON *body = fp_hello_build(&h);   /* duplicates sch/kids */
    cJSON_Delete(sch);
    cJSON_Delete(kids);

    cJSON *msg = fp_envelope_build(FP_MSG_HELLO, body, s->cfg->serial,
                                   "hello-1", NULL, fp_now_ms());
    FP_LOGI(TAG, "→ hello");
    return send_msg(s, msg);
}

static bool ota_check_send(fp_session_t *s) {
    fp_ota_check_t o = {0};
    o.current_version = s->cfg->fw_version; o.has_current_version = true;
    if (s->cfg->hw_rev) { o.hw_rev = s->cfg->hw_rev; o.has_hw_rev = true; }
    cJSON *body = fp_ota_check_build(&o);
    cJSON *msg = fp_envelope_build(FP_MSG_OTA_CHECK, body, s->cfg->serial,
                                   "chk-1", NULL, fp_now_ms());
    if (!sign_c2s(s, msg)) { cJSON_Delete(msg); return false; }
    FP_LOGI(TAG, "→ ota_check [signed seq=%ld] (session proof)", s->c2s_seq);
    return send_msg(s, msg);
}

/* ----- periodic senders ----------------------------------------------- */
bool fp_session_heartbeat_send(fp_session_t *s, uint32_t uptime_s) {
    if (!s->running) return false;
    fp_heartbeat_t h = {0};
    h.uptime_s = uptime_s; h.has_uptime_s = true;
    h.fw_version = s->cfg->fw_version; h.has_fw_version = true;
    h.fault_active = false; h.has_fault_active = true;
    cJSON *body = fp_heartbeat_build(&h);
    cJSON *msg = fp_envelope_build(FP_MSG_HEARTBEAT, body, s->cfg->serial,
                                   NULL, NULL, fp_now_ms());
    return send_msg(s, msg);
}

/* names == NULL -> full snapshot; otherwise only the requested datapoints. */
static bool dp_report_send_names(fp_session_t *s, const char *in_reply_to,
                                 const cJSON *names) {
    cJSON *dp = cJSON_CreateObject();
    if (s->cfg->fill_snapshot) s->cfg->fill_snapshot(dp, names, s->cfg->user);
    fp_dp_report_t r = {0};
    r.seq = s->report_seq++; r.has_seq = true;
    r.dp = dp; r.has_dp = true;
    cJSON *body = fp_dp_report_build(&r);    /* duplicates dp */
    cJSON_Delete(dp);
    cJSON *msg = fp_envelope_build(FP_MSG_DP_REPORT, body, s->cfg->serial,
                                   NULL, in_reply_to, fp_now_ms());
    return send_msg(s, msg);
}

bool fp_session_dp_report_send(fp_session_t *s, const char *in_reply_to) {
    return dp_report_send_names(s, in_reply_to, NULL);
}

/* On-change report: sends a dp_report carrying ONLY the changed points
 * (from cfg->fill_changes). Returns true also when nothing changed. */
bool fp_session_dp_changes_send(fp_session_t *s) {
    if (!s->cfg->fill_changes) return true;
    cJSON *dp = cJSON_CreateObject();
    int n = s->cfg->fill_changes(dp, s->cfg->user);
    if (n <= 0) { cJSON_Delete(dp); return true; }
    fp_dp_report_t r = {0};
    r.seq = s->report_seq++; r.has_seq = true;
    r.dp = dp; r.has_dp = true;
    cJSON *body = fp_dp_report_build(&r);    /* duplicates dp */
    cJSON_Delete(dp);
    cJSON *msg = fp_envelope_build(FP_MSG_DP_REPORT, body, s->cfg->serial,
                                   NULL, NULL, fp_now_ms());
    return send_msg(s, msg);
}

/* ----- incoming messages ------------------------------------------- */
static void handle_hello_ack(fp_session_t *s, const cJSON *m) {
    const cJSON *acc = cJSON_GetObjectItemCaseSensitive(m, "accepted");
    if (!cJSON_IsTrue(acc)) {
        FP_LOGW(TAG, "hello_ack accepted=false (%s)", str_of(m, "reason"));
        return;
    }
    const char *sn = str_of(m, "server_nonce");
    const char *kid = str_of(m, "auth_kid");
    if (!sn || !kid) { FP_LOGE(TAG, "hello_ack without server_nonce/auth_kid"); return; }
    snprintf(s->server_nonce, sizeof s->server_nonce, "%s", sn);
    snprintf(s->kid, sizeof s->kid, "%s", kid);
    s->negotiated = true;
    FP_LOGI(TAG, "← hello_ack accepted (scope=%s, kid=%s) → v2 ok",
            str_of(m, "auth_scope"), s->kid);
    ota_check_send(s);
}

static void handle_dp_read(fp_session_t *s, const cJSON *m) {
    /* names == [] means full snapshot (spec §7.x); otherwise only the named ones. */
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(m, "names");
    if (names && (!cJSON_IsArray(names) || cJSON_GetArraySize(names) == 0))
        names = NULL;
    dp_report_send_names(s, str_of(m, "msg_id"), names);
}

/* log_read / log_read_prev (auth=control): the app callback fills the
 * log_batch body from the logging component's ring/flash tier. */
static void handle_log_read(fp_session_t *s, const cJSON *m, bool prev_boot) {
    if (!verify_s2c(s, m)) return;

    uint32_t since = 0;
    uint8_t  min_level = 0;                 /* 0 = no level filter */
    uint16_t max_records = 64;
    const cJSON *it;
    if ((it = cJSON_GetObjectItemCaseSensitive(m, "since_seq")) && cJSON_IsNumber(it))
        since = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItemCaseSensitive(m, "min_level")) && cJSON_IsNumber(it))
        min_level = (uint8_t)it->valuedouble;
    if ((it = cJSON_GetObjectItemCaseSensitive(m, "max_records")) && cJSON_IsNumber(it)) {
        max_records = (uint16_t)it->valuedouble;
        if (max_records > 128) max_records = 128;   /* WS frame bound */
    }

    cJSON *body = cJSON_CreateObject();
    if (s->cfg->fill_log_batch) {
        s->cfg->fill_log_batch(body, since, min_level, max_records,
                               prev_boot, s->cfg->user);
    } else {
        cJSON_AddNumberToObject(body, "boot_id", 0);
        cJSON_AddItemToObject(body, "records", cJSON_CreateArray());
    }
    cJSON *msg = fp_envelope_build(FP_MSG_LOG_BATCH, body, s->cfg->serial,
                                   NULL, str_of(m, "msg_id"), fp_now_ms());
    send_msg(s, msg);
}

/* history_read (auth=control): read back the pressure history — log_read pattern. */
static void handle_history_read(fp_session_t *s, const cJSON *m) {
    if (!verify_s2c(s, m)) return;

    uint32_t since = 0;
    uint32_t max_samples = 100;
    const cJSON *it;
    if ((it = cJSON_GetObjectItemCaseSensitive(m, "since_seq")) && cJSON_IsNumber(it))
        since = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItemCaseSensitive(m, "max_samples")) && cJSON_IsNumber(it)) {
        max_samples = (uint32_t)it->valuedouble;
        if (max_samples > 120) max_samples = 120;   /* local 4-KB frame limit */
    }

    cJSON *body = cJSON_CreateObject();
    if (s->cfg->fill_history_batch) {
        s->cfg->fill_history_batch(body, since, max_samples, s->cfg->user);
    } else {
        cJSON_AddItemToObject(body, "samples", cJSON_CreateArray());
        cJSON_AddNumberToObject(body, "next_seq", 1);
    }
    cJSON *msg = fp_envelope_build(FP_MSG_HISTORY_BATCH, body, s->cfg->serial,
                                   NULL, str_of(m, "msg_id"), fp_now_ms());
    send_msg(s, msg);
}

static void handle_log_ack_prev(fp_session_t *s, const cJSON *m) {
    if (!verify_s2c(s, m)) return;
    uint32_t boot_id = 0;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(m, "boot_id");
    if (it && cJSON_IsNumber(it)) boot_id = (uint32_t)it->valuedouble;

    bool ok = s->cfg->on_log_ack_prev
                  ? s->cfg->on_log_ack_prev(boot_id, s->cfg->user) : false;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", ok);
    cJSON *msg = fp_envelope_build(FP_MSG_LOG_ACK_RESULT, body, s->cfg->serial,
                                   NULL, str_of(m, "msg_id"), fp_now_ms());
    send_msg(s, msg);
}

static void handle_command(fp_session_t *s, const cJSON *m) {
    const char *mid = str_of(m, "msg_id");
    const char *cmd = str_of(m, "command");
    cJSON *result = cJSON_CreateObject();
    if (verify_s2c(s, m)) {
        if (s->cfg->on_command) {
            s->cfg->on_command(m, result, s->cfg->user);
        } else {
            cJSON_AddStringToObject(result, "status", "rejected");
            cJSON_AddStringToObject(result, "error", "unknown_command");
        }
    } else {
        cJSON_AddStringToObject(result, "status", "rejected");
        cJSON_AddStringToObject(result, "error", "auth_failed");
    }
    if (!cJSON_GetObjectItem(result, "command"))
        cJSON_AddStringToObject(result, "command", cmd ? cmd : "");
    cJSON *msg = fp_envelope_build(FP_MSG_COMMAND_RESULT, result,
                                   s->cfg->serial, NULL, mid, fp_now_ms());
    send_msg(s, msg);
}

static void handle_dp_write(fp_session_t *s, const cJSON *m) {
    const char *mid = str_of(m, "msg_id");
    cJSON *result = cJSON_CreateObject();
    if (verify_s2c(s, m)) {
        const cJSON *dp = cJSON_GetObjectItemCaseSensitive(m, "dp");
        if (s->cfg->on_dp_write) {
            s->cfg->on_dp_write(dp, result, s->cfg->user);
        } else {
            cJSON_AddStringToObject(result, "status", "rejected");
        }
    } else {
        cJSON_AddStringToObject(result, "status", "rejected");
        cJSON *errs = cJSON_AddObjectToObject(result, "errors");
        cJSON_AddStringToObject(errs, "_", "auth_failed");
    }
    cJSON *msg = fp_envelope_build(FP_MSG_DP_WRITE_RESULT, result,
                                   s->cfg->serial, NULL, mid, fp_now_ms());
    send_msg(s, msg);
}

bool fp_session_message_handle(fp_session_t *s, const char *json) {
    cJSON *m = cJSON_Parse(json);
    if (!m) { FP_LOGW(TAG, "invalid JSON received"); return false; }
    fp_msg_type_t t = fp_envelope_type(m);

    switch (t) {
        case FP_MSG_HELLO_ACK: handle_hello_ack(s, m); break;
        case FP_MSG_OTA_NONE:
            s->running = true;
            FP_LOGI(TAG, "<- ota_none -> operation starts");
            if (s->cfg->on_ready) s->cfg->on_ready(s->cfg->user);
            break;
        case FP_MSG_DP_READ:  handle_dp_read(s, m);  break;
        /* Control messages (writes/commands/log pull) only in the RUNNING
         * state (after ota_none) — not already after hello_ack. Otherwise
         * they are ignored (no half-established write access). */
        case FP_MSG_COMMAND:  if (s->running) handle_command(s, m);  break;
        case FP_MSG_DP_WRITE: if (s->running) handle_dp_write(s, m); break;
        case FP_MSG_LOG_READ:      if (s->running) handle_log_read(s, m, false); break;
        case FP_MSG_LOG_READ_PREV: if (s->running) handle_log_read(s, m, true);  break;
        case FP_MSG_LOG_ACK_PREV:  if (s->running) handle_log_ack_prev(s, m);    break;
        case FP_MSG_HISTORY_READ:  if (s->running) handle_history_read(s, m);    break;
        case FP_MSG_OTA_AVAILABLE:
            /* MAC must be verified (scope=control); only then is the delivered
             * sha256/size/url server-attested (§H). Only afterwards start the OTA. */
            if (verify_s2c(s, m)) {
                FP_LOGI(TAG, "← ota_available (verified) → on_ota");
                if (s->cfg->on_ota) s->cfg->on_ota(m, s->cfg->user);
            }
            break;
        case FP_MSG_OTA_CANCEL:
            if (verify_s2c(s, m))
                FP_LOGI(TAG, "<- ota_cancel (verified; cancellation not implemented)");
            break;
        default:
            FP_LOGI(TAG, "← %s (unhandled)", fp_msg_type_name(t));
            break;
    }
    cJSON_Delete(m);
    return true;
}
