/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "cJSON.h"

/* =============================================================
 * ota_task module — performs an OTA update when the server sends a
 * (verified) ota_available. Runs in its own
 * FreeRTOS task so the session/heartbeats do not block.
 *
 * The source of the metadata (url, target_version, sha256, size, crc32) is the
 * server-attested ota_available message (scope=control, MAC checked).
 * Progress/result is reported via fountain_proto_ota_status_send().
 * ============================================================= */

/* Starts the OTA download/flash based on the ota_available message.
 * `pstOtaMsg` is read only during the call (fields are copied). */
void ota_start(const cJSON *pstOtaMsg);
