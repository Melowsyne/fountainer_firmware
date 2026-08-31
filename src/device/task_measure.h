/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "esp_err.h"
#include "task_manager.h"

/* =============================================================
 * task_measure module — cyclic DEVICE sensing, run as callbacks
 * under the task_manager component (TM_TASK_MEASURE in
 * src/main/task_table.c; loop/timing/stop live there).
 *   - every cycle (5 s): pressure/mV (hal), relay mirror, chip
 *     temp, pump state machine tick, edge-triggered alerts
 *   - every 60 s (sub-period): temperature + humidity (am2302)
 * Writes into data_store/datapoints; distribution via observers.
 * ============================================================= */

#define MEASURE_PRESSURE_PERIOD_MS   5000
#define MEASURE_CLIMATE_PERIOD_MS    60000

esp_err_t measure_task_start(tm_task_ctx_t *pstCtx);
esp_err_t measure_task_cycle(tm_task_ctx_t *pstCtx);
esp_err_t measure_task_stop(tm_task_ctx_t *pstCtx);

/* TEST ONLY (wd_fault command): block the NEXT cycle for ulSeconds — lets
 * the WD_MEASURE channel and the TWDT fallback be exercised on demand. */
void task_measure_test_block_set(uint32_t ulSeconds);
