/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * local_session — session slots of the local WSS server
 * (firmware_server.md §13-§14, §28, §56). Statically allocated; the count
 * is the hard RAM limiter (implementation plan: start with 2, 5 only after
 * a measurement campaign).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fp_session.h"
#include "local_rate_limit.h"

/* Concurrent local maintenance sessions. Default 1: in practice a maintenance
 * interface serves one client; this keeps RAM free (~18 KB per session
 * with MBEDTLS_DYNAMIC_BUFFER) and categorically rules out two concurrent
 * LOCAL writers. Can be raised at build time via -DLOCAL_SERVER_MAX_CLIENTS=<n>
 * (1..5) (M1: RAM is the bottleneck; measure >2 before release).
 * CAUTION with 1: if a client hangs half-open, the device is locally
 * unreachable until the idle timeout (LOCAL_IDLE_TIMEOUT_US, 5 min). */
#ifndef LOCAL_SERVER_MAX_CLIENTS
#define LOCAL_SERVER_MAX_CLIENTS 1
#endif
#if (LOCAL_SERVER_MAX_CLIENTS < 1) || (LOCAL_SERVER_MAX_CLIENTS > 5)
#error "LOCAL_SERVER_MAX_CLIENTS must be between 1 and 5 (RAM/handshake)."
#endif

typedef enum {
    LOCAL_SESSION_FREE = 0,
    LOCAL_SESSION_CONNECTED,      /* WS open, Fountain hello in flight    */
    LOCAL_SESSION_RUNNING,        /* Fountain session running (ota_none)  */
    LOCAL_SESSION_CLOSING,
} local_session_state_t;

typedef enum {
    LOCAL_CLOSE_NORMAL = 0,
    LOCAL_CLOSE_WIFI_LOST,
    LOCAL_CLOSE_RATE_LIMIT,
    LOCAL_CLOSE_FRAME_TOO_LARGE,
    LOCAL_CLOSE_QUEUE_OVERFLOW,
    LOCAL_CLOSE_PROTOCOL_ERROR,
    LOCAL_CLOSE_TIMEOUT,
    LOCAL_CLOSE_SERVER_STOP,
} local_close_reason_t;

typedef struct {
    bool     active;
    uint32_t connection_id;       /* generation — invalidates old descriptors */
    int      sockfd;
    local_session_state_t state;
    int64_t  connected_us;
    int64_t  last_activity_us;

    fp_session_t       protocol;  /* own Fountain state (AP5)              */
    local_rate_limit_t rate;

    uint8_t  json_strikes;        /* invalid messages (§33)                */
} local_session_t;

/* Statistics (AP6 mirrors them into Local_* datapoints). */
typedef struct {
    uint32_t connections_total;
    uint32_t rate_drops;
    uint32_t auth_failures;       /* TLS level is counted by esp-tls; Fountain here */
    uint32_t queue_overflows;
    uint32_t forced_disconnects;
    uint32_t frames_rx;
} local_server_stats_t;
