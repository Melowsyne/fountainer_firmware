/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "esp_err.h"
#include "app_watchdog.h"

/* =============================================================
 * watchdog_table — the PROJECT's supervision channels for the
 * generic app_watchdog component (Watchdog_v1.md).
 *
 * | channel    | heartbeat source (after success)   | deadline | recovery |
 * | WD_SESSION | fp_task on_alive / session ready   |   180 s  | 5x WS
 * |            |                                    |          | restart,
 * |            |                                    |          | then
 * |            |                                    |          | reboot if
 * |            |                                    |          | link up
 * | WD_MEASURE | completed measure cycle            |    15 s  | 1x warn,
 * |            |                                    |          | then
 * |            |                                    |          | reboot
 * | WD_MONITOR | completed monitor cycle            |    15 s  | ditto
 * | WD_EVENT   | EVT_MONITOR_TICK subscriber (runs  |    30 s  | ditto
 * |            | in the event dispatch task)        |          |
 *
 * Semantics of WD_SESSION replicate the LIVE-TESTED two-stage
 * connectivity watchdog (2026-07-06): restarts on a 180-s grid,
 * reboot after 5 fruitless restarts and only while the physical
 * link is up — now with the RTC reboot brake on top.
 * ============================================================= */

enum {
    WD_CH_SESSION = 0,
    WD_CH_MEASURE,
    WD_CH_MONITOR,
    WD_CH_EVENT,
    WD_CH_PUMP,          /* critical: 200-ms control cycle */

    WD_CH_COUNT
};

/* Registers all channels + injects the reboot hook; after event/task
 * managers, before wd_start(). */
esp_err_t watchdog_table_register_all(void);
