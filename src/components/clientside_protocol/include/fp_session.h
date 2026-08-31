/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* fp_session — protocol state machine of the client side.
 *
 * Pure logic (no WebSocket, no FreeRTOS): handshake/negotiation, auth
 * (sign the session proof, verify incoming s2c control), dispatch to
 * the application callbacks and building the responses. The transport (sending) is
 * injected as a function pointer -> host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "fp_auth.h"
#include "fountain_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Transport: sends a JSON text frame. true on success. */
typedef bool (*fp_transport_send_t)(void *ctx, const char *json);

typedef struct {
    const fp_config_t  *cfg;
    fp_transport_send_t send;
    void               *send_ctx;

    /* Runtime (per connection) */
    char        server_nonce[64];
    char        client_nonce[32];
    char        kid[16];
    fp_replay_t replay_s2c;     /* anti-replay of incoming signed s2c */
    long        c2s_seq;        /* signed c2s (session proof) */
    uint32_t    report_seq;     /* dp_report sequence */
    bool        negotiated;
    bool        running;        /* after ota_none */
} fp_session_t;

void fp_session_init(fp_session_t *s, const fp_config_t *cfg,
                     fp_transport_send_t send, void *send_ctx);
void fp_session_reset(fp_session_t *s);          /* per new connection */
void fp_session_close(fp_session_t *s);          /* connection end: wipes
                                                  * nonces/kid (zeroization) */

/* NOTE (multi-session): c2s request msg_ids are the literals "hello-1"/
 * "chk-1" — unique per CONNECTION, which is the protocol correlation scope.
 * A per-session counter is deliberately deferred (plan: firmware_server). */

bool fp_session_hello_send(fp_session_t *s);                 /* on WS connect */
bool fp_session_message_handle(fp_session_t *s, const char *json);  /* on RX */
bool fp_session_heartbeat_send(fp_session_t *s, uint32_t uptime_s);
bool fp_session_dp_report_send(fp_session_t *s, const char *in_reply_to);
bool fp_session_dp_changes_send(fp_session_t *s);   /* on-change (see cfg) */

static inline bool fp_session_running(const fp_session_t *s) { return s->running; }
static inline bool fp_session_negotiated(const fp_session_t *s) { return s->negotiated; }

/* A FAILED/deferred OTA must not leave the session in the negotiated-but-
 * not-running limbo (no reports/heartbeats until the session watchdog
 * recovers it after 180 s): the update path ends here, normal operation
 * starts exactly as if the server had sent ota_none. */
static inline bool fp_session_ota_failed_note(fp_session_t *s)
{
    if (!s->negotiated || s->running) return false;
    s->running = true;
    return true;                      /* caller fires on_ready */
}

#ifdef __cplusplus
}
#endif
