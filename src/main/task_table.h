/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "esp_err.h"
#include "task_manager.h"

/* =============================================================
 * task_table — the PROJECT's cyclic tasks under task_manager
 * control (the component itself is project-agnostic).
 *
 * Managed here: task_measure (device sensing + pump tick) and
 * main_monitor (system metrics, power tick, supervision).
 * NOT managed: fp_task/fp_tx (protocol component), the WS client
 * task, com_starter, one-shot tasks (OTA, reboot).
 * ============================================================= */

enum {
    TM_TASK_MEASURE = 0,
    TM_TASK_MONITOR,
    TM_TASK_PUMP,          /* pump_task: 200-ms pressure/state-machine cycle */

    TM_TASK_COUNT
};

/* Registers all configured tasks; call once after tm_init(). */
esp_err_t task_table_register_all(void);
