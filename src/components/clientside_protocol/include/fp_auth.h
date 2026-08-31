/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* fp_auth — v2.2 message authentication (HMAC-SHA256) for the client side.
 *
 * Byte-identical to the server side (serverside_protocol/fountain_proto/auth.py) and to
 * the golden test vector in fountain_proto_schema/AUTH-CONTRACT.md.
 *
 * Deliberately free of ESP-IDF/FreeRTOS dependencies (only cJSON + mbedTLS), so
 * this module is host-testable (see test/host/).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical body (JCS-like): keys sorted, no whitespace, integers short.
 * Return value is malloc'd, to be freed by the caller. */
char *fp_auth_canon_build(const cJSON *node);

/* SHA-256 -> 64 hex chars (lowercase) in out (>=65). */
void fp_auth_sha256_hex(const char *data, size_t len, char out[65]);

/* body_hash of the message: remove envelope+auth, canonicalize, sha256-hex. */
void fp_auth_body_hash_build(const cJSON *msg, char out[65]);

/* base64 of n bytes -> malloc'd string. */
char *fp_auth_base64_encode(const uint8_t *data, size_t n);

/* Hex -> bytes; returns the byte count, -1 on error. */
int fp_auth_hex_decode(const char *hex, uint8_t *out, size_t outcap);

/* Signs msg: appends msg["auth"]={kid,seq,mac}. true on success. */
bool fp_auth_message_sign(cJSON *msg, const uint8_t *key, size_t keylen,
                          const char *kid, long seq, const char *direction,
                          const char *device_id, const char *server_nonce,
                          const char *client_nonce);

/* Checks msg["auth"] (only MAC/authenticity, NO seq comparison). 1 = ok. */
int fp_auth_message_verify(const cJSON *msg, const uint8_t *key, size_t keylen,
                           const char *expected_kid, const char *direction,
                           const char *device_id, const char *server_nonce,
                           const char *client_nonce, const char **reason);

/* Anti-replay: strictly increasing seq per (session, direction). */
typedef struct { long last; } fp_replay_t;
static inline void fp_replay_reset(fp_replay_t *r) { r->last = 0; }
bool fp_replay_check(fp_replay_t *r, long seq);   /* true if seq > last (adopts it) */

#ifdef __cplusplus
}
#endif
