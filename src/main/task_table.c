/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "task_table.h"
#include "task_measure.h"
#include "pump_task.h"
#include "app_watchdog.h"
#include "watchdog_table.h"

/* Monitor callback lives in main.c (same module). */
extern esp_err_t main_monitor_cycle(tm_task_ctx_t *pstCtx);

/* Cycle wrappers: report the app-watchdog heartbeat ONLY after a completed,
 * successful cycle (channel IDs are main knowledge — the device layer must
 * not include watchdog_table). */
static esp_err_t measure_cycle_supervised(tm_task_ctx_t *pstCtx)
{
    esp_err_t err = measure_task_cycle(pstCtx);
    if (err == ESP_OK) wd_heartbeat(WD_CH_MEASURE);
    return err;
}

static esp_err_t monitor_cycle_supervised(tm_task_ctx_t *pstCtx)
{
    esp_err_t err = main_monitor_cycle(pstCtx);
    if (err == ESP_OK) wd_heartbeat(WD_CH_MONITOR);
    return err;
}

static esp_err_t pump_cycle_supervised(tm_task_ctx_t *pstCtx)
{
    esp_err_t err = pump_task_cycle(pstCtx);
    if (err == ESP_OK) wd_heartbeat(WD_CH_PUMP);
    return err;
}

/* twdt_subscribe is safe now: work package 4 raises the TWDT timeout to
 * 60 s (PANIC), well above the 5-s periods + cycle budgets. */
static const tm_periodic_task_def_t s_astDefs[TM_TASK_COUNT] = {
    [TM_TASK_MEASURE] = {
        .name           = "task_measure",
        .on_start       = measure_task_start,
        .on_cycle       = measure_cycle_supervised,
        .on_stop        = measure_task_stop,
        .user_arg       = NULL,
        .period_ms      = MEASURE_PRESSURE_PERIOD_MS,   /* 5000 ms */
        .max_runtime_ms = 2000,      /* DHT22 + I2C + statemachine budget */
        .stack_size     = 4096,
        .priority       = 4,
        .core_id        = 1,
        .twdt_subscribe = true,
    },
    [TM_TASK_MONITOR] = {
        .name           = "main_monitor",
        .on_start       = NULL,
        .on_cycle       = monitor_cycle_supervised,
        .on_stop        = NULL,
        .user_arg       = NULL,
        .period_ms      = 5000,
        .max_runtime_ms = 1000,
        .stack_size     = 4096,
        .priority       = 3,
        .core_id        = 0,
        .twdt_subscribe = true,
    },
    [TM_TASK_PUMP] = {
        .name           = "pump_task",
        .on_start       = NULL,          /* pump_task_init runs in main_init */
        .on_cycle       = pump_cycle_supervised,
        .on_stop        = NULL,
        .user_arg       = NULL,
        .period_ms      = PUMP_TASK_PERIOD_MS,   /* 200 ms (5 Hz pressure) */
        .max_runtime_ms = 100,           /* ADS1115 single shot ~10 ms      */
        .stack_size     = 4096,
        .priority       = 6,             /* control beats telemetry         */
        .core_id        = 1,
        .twdt_subscribe = true,
    },
};

esp_err_t task_table_register_all(void)
{
    for (tm_task_id_t id = 0; id < TM_TASK_COUNT; id++) {
        esp_err_t err = tm_register_periodic(id, &s_astDefs[id]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
