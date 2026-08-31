/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================
 * task_manager — generic lifecycle/periodics layer on top of
 * FreeRTOS for cyclic application tasks (TaskManager_v1.md).
 *
 * Application logic is provided as a callback triple instead of
 * an own endless loop; the manager owns the loop, the fixed time
 * grid (xTaskDelayUntil), cooperative stopping and SUPERVISION:
 * per-cycle runtime budget and stack watermark are checked and
 * violations published via the event_manager
 * (EVT_TASK_CYCLE_OVERRUN / EVT_TASK_STACK_LOW).
 *
 * NOT managed here: the protocol component's tasks (fp_task/
 * fp_tx), the WS client task, one-shot tasks (OTA, reboot).
 * The PROJECT task table lives in src/main/task_table.c —
 * this component stays free of project knowledge.
 * ============================================================= */

#define TASK_MANAGER_MAX_TASKS       8
#define TM_STACK_LOW_WATERMARK_WORDS 128   /* EVT_TASK_STACK_LOW threshold */

/* Project task IDs live in src/main/task_table.h; the component only needs
 * the numeric range [0, TASK_MANAGER_MAX_TASKS). */
typedef uint32_t tm_task_id_t;

typedef struct tm_task_ctx tm_task_ctx_t;
typedef esp_err_t (*tm_task_callback_t)(tm_task_ctx_t *ctx);

typedef enum {
    TM_STATE_UNUSED = 0,
    TM_STATE_REGISTERED,
    TM_STATE_RUNNING,
    TM_STATE_STOPPING,
    TM_STATE_STOPPED
} tm_task_state_t;

typedef struct {
    const char *name;

    tm_task_callback_t on_start;   /* optional, once at task start          */
    tm_task_callback_t on_cycle;   /* required, every period_ms; MUST return */
    tm_task_callback_t on_stop;    /* optional, on cooperative stop          */

    void *user_arg;

    uint32_t period_ms;
    uint32_t max_runtime_ms;       /* per-cycle budget; 0 = unlimited        */
    uint32_t stack_size;           /* bytes                                  */
    UBaseType_t priority;
    BaseType_t core_id;            /* 0 / 1 / tskNO_AFFINITY                 */
    bool twdt_subscribe;           /* register task with the ESP-IDF TWDT.
                                      Leave false until the TWDT timeout is
                                      raised above the longest period
                                      (work package 4)!                      */
} tm_periodic_task_def_t;

esp_err_t tm_init(void);
esp_err_t tm_register_periodic(tm_task_id_t id, const tm_periodic_task_def_t *def);
esp_err_t tm_start(tm_task_id_t id);
esp_err_t tm_stop(tm_task_id_t id, TickType_t timeout);
esp_err_t tm_start_all(void);
esp_err_t tm_stop_all(TickType_t timeout_per_task);

/* For callbacks. */
bool         tm_should_stop(tm_task_ctx_t *ctx);
void        *tm_get_arg(tm_task_ctx_t *ctx);
tm_task_id_t tm_get_id(tm_task_ctx_t *ctx);

/* For supervision (watchdog, package 4) and diagnostics.
 * CONTRACT: tm_get_handle is diagnosis-only — the handle may turn NULL
 * concurrently on a cooperative stop; callers must NULL-check and never
 * hold it across cycles. */
TaskHandle_t    tm_get_handle(tm_task_id_t id);
tm_task_state_t tm_get_state(tm_task_id_t id);
uint32_t        tm_get_last_cycle_ms(tm_task_id_t id);
UBaseType_t     tm_get_stack_watermark(tm_task_id_t id);

#ifdef __cplusplus
}
#endif
