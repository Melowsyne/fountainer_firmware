/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

/* =============================================================
 * link_quality — link supervision (Link_Robustness_v1, stage 1+3).
 * Collects RSSI/disconnects/session-drops/TX-fails, runs the pure
 * link_score, mirrors the Net_* datapoints, emits the NET_SAMPLE
 * time-series record (60 s GOOD / 120 s POOR), the edge records
 * LINK_POOR/RECOVERED (+ device_alert once a session is up) and
 * publishes EVT_LINK_STATE_CHANGED for consumers in other layers
 * (pump_task sample throttling, main's slow-mode/PS coordination).
 * Ticked by the monitor cycle (5 s).
 * ============================================================= */

bool link_quality_init(void);          /* subscribes session events        */
void link_quality_tick(void);          /* from main_monitor_cycle (5 s)    */

/* Current hysteresis state — consumed by task_com (dynamic log_batch
 * budget) and ota_task (download gate). */
bool link_quality_poor_get(void);

/* TEST ONLY (link_fault command): force the POOR state for ulSeconds so
 * the whole adaptation chain (records, alert, slow grid, PS suspend,
 * batch budget, OTA gate) can be exercised on a healthy testbed link. */
void link_quality_test_poor_set(uint32_t ulSeconds);
