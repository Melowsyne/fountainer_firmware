/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

/* =============================================================
 * task_com module — runs as a FreeRTOS task ("task" in the name).
 * Performs the actual communication with the Linux server:
 *   - manage esp_websocket_client (own internal task for TLS/frames)
 *   - connection/negotiation flow (hello -> hello_ack -> negotiate)
 *   - single TX path via a tx_queue (no concurrent sends)
 *   - decode incoming (protocol_com) and dispatch (e.g. to command)
 *   - NEGOTIATED gate: app messages only after successful negotiation
 *
 * Patterns: STATE (connection flow via protocol_com),
 *           PRODUCER/CONSUMER (tx_queue), OBSERVER consumer (wlan state CB).
 * ============================================================= */

bool task_com_start(void);

/* Session-progress hook (injected by main): called ~1/s while the protocol
 * session is negotiated & running — feeds the WD_SESSION heartbeat without
 * the network layer knowing the watchdog channel IDs (main knowledge). */
void task_com_alive_hook_set(void (*pfnAlive)(void));

/* Embedded TLS material (NULL when not provisioned -> plaintext fallback).
 * Shared with ota_task for the https firmware download (mutual TLS). */
const char *task_com_tls_ca_get(void);
const char *task_com_tls_client_cert_get(void);
const char *task_com_tls_client_key_get(void);

/* Shared application logic for dp_write/command/log_read — WITHOUT
 * power_mgmt_activity_note and without cloud-session state. This lets the
 * local maintenance session (local_protocol.c) use exactly the same semantics
 * as the cloud (identical feature set) without breaking the read-only
 * isolation contract (no keep-awake, no cloud events, no on-change baseline).
 * Signatures of the last two = fp_config callbacks (directly wireable). */
void task_com_apply_dp_write(const cJSON *dp, cJSON *result_out);
void task_com_apply_command(const cJSON *cmd_msg, cJSON *result_out);
void task_com_fill_log_batch(cJSON *body_out, uint32_t since_seq,
                             uint8_t min_level, uint16_t max_records,
                             bool prev_boot, void *user);
bool task_com_log_ack_prev(uint32_t boot_id, void *user);
void task_com_fill_history_batch(cJSON *body_out, uint32_t since_seq,
                                 uint32_t max_samples, void *user);
