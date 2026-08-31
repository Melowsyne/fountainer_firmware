/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* Host unit test: reproduces the golden auth vector from
 * fountain_proto_schema/AUTH-CONTRACT.md with fp_auth.c (mbedTLS).
 * Build: see test/host/README.md. */
#include "fp_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *KEYHEX =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const char *EXP_BH =
    "df69a908821ed289cbaf08d81b4ae7369a6099afb684e1925890053cd255e9e2";
static const char *EXP_MAC = "QsNu1LP0C0yOt5Gvftvbzg==";
static const char *DID = "esp32-a1b2c3d4e5f6";
static const char *SNONCE = "9f3aK2pL0xQ7sV4nB1dC8g==";
static const char *CNONCE = "Yt8m1Q5fT3oQh2bJ0a9w7w==";

int main(void) {
    uint8_t key[32];
    int kl = fp_auth_hex_decode(KEYHEX, key, sizeof key);

    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "v", 2);
    cJSON_AddStringToObject(m, "type", "command");
    cJSON_AddNumberToObject(m, "ts", 1718370040000.0);
    cJSON_AddStringToObject(m, "msg_id", "cmd-7");
    cJSON_AddStringToObject(m, "command", "set_state");
    cJSON_AddStringToObject(m, "target_state", "On");

    char bh[65];
    fp_auth_body_hash_build(m, bh);
    printf("body_hash      : %s\n", bh);

    fp_auth_message_sign(m, key, (size_t)kl, "1", 1, "s2c", DID, SNONCE, CNONCE);
    const char *mac =
        cJSON_GetObjectItem(cJSON_GetObjectItem(m, "auth"), "mac")->valuestring;
    printf("mac            : %s\n", mac);

    int ok = (strcmp(bh, EXP_BH) == 0) && (strcmp(mac, EXP_MAC) == 0);
    printf("GOLDEN VECTOR  : %s\n", ok ? "OK" : "MISMATCH");

    const char *reason = "?";
    int v = fp_auth_message_verify(m, key, (size_t)kl, "1", "s2c", DID,
                                   SNONCE, CNONCE, &reason);
    printf("verify         : %s\n", v ? "ok" : reason);

    /* Replay logic */
    fp_replay_t r = {0};
    int replay_ok = fp_replay_check(&r, 1) && !fp_replay_check(&r, 1) &&
                    fp_replay_check(&r, 2) && !fp_replay_check(&r, 2);
    printf("anti_replay    : %s\n", replay_ok ? "ok" : "FAIL");

    cJSON_Delete(m);
    return (ok && v && replay_ok) ? 0 : 1;
}
