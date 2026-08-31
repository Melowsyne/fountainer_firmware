/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "task_manager.h"
#include "pump_manager.h"

/* =============================================================
 * pump_task — the I/O binding of the pure pump_manager, run as
 * callbacks under the task_manager (TM_TASK_PUMP, 200 ms):
 *   read pressure (hal/ADS1115, 5 Hz) -> pm_update -> apply the
 *   relay (hal) -> mirror datapoints -> publish state/fault
 *   events -> log records (1-Hz sample @DEBUG, event summaries,
 *   faults) -> device_alerts -> poll Fon_Fault_Ack/_Event_Label.
 * Config datapoints (Fon_*) are re-applied once per second; the
 * sensor curve feeds hal_pressure_calibration_set (range keeps
 * the original 500-PSI value, changeable per datapoint).
 * Thread-safety: remote requests (command module, WS context)
 * and the cycle share one mutex around the pm instance.
 * ============================================================= */

#define PUMP_TASK_PERIOD_MS 200

bool pump_task_init(void);   /* pm instance + config from datapoints */

esp_err_t pump_task_cycle(tm_task_ctx_t *pstCtx);

/* Remote requests (called from the command module / protocol context). */
bool pump_request_on(uint32_t ulMaxDurationS);   /* 0 = unlimited */
bool pump_request_off(void);
bool pump_request_restart(void);
bool pump_mode_set(pm_mode_t eMode);

/* Probes. */
pm_state_t pump_state_get(void);
bool       pump_idle_get(void);          /* power_mgmt provider */
