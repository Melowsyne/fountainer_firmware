/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * local_server — local WSS server on the device (firmware_server.md).
 *
 * wss://<device-ip>:4443/ws, subprotocol "fountain", mTLS enforced
 * (root CA verifies client certificates). The ESP32 logically remains the
 * Fountain DEVICE: for every accepted connection IT starts the hello
 * handshake; the maintenance client plays the Fountain SERVER role.
 *
 * Lifecycle: local_server_init() once at boot; start/stop follows the
 * WLAN (EVT_WLAN_CONNECTED/DISCONNECTED). Read-only phase: dp_write/
 * command/log_read are answered by the session automatically with "rejected"
 * (NULL callbacks in the local fp_config_t).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "local_session.h"

#define LOCAL_SERVER_PORT      4443
#define LOCAL_RX_QUEUE_DEPTH   8
#define LOCAL_IDLE_TIMEOUT_US  (300 * 1000 * 1000LL)   /* 5 min without a frame */

/* Once at boot (after factory_config/task_com init, before wd_start):
 * creates slots/queues/pool and subscribes to the WLAN events. */
esp_err_t local_server_init(void);

bool local_server_running(void);
void local_server_stats_get(local_server_stats_t *out);
uint8_t local_server_client_count(void);

/* TX path for the protocol layer (AP5): serialized through the
 * httpd task (one TX owner per socket, §41). conn_id invalidates sends to
 * a slot that has since been reassigned. */
bool local_server_send(uint8_t slot, uint32_t conn_id, const char *json);
bool local_server_session_send(local_session_t *s, const char *json);
void local_server_session_close(local_session_t *s, local_close_reason_t eReason);
