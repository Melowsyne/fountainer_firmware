/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* =============================================================
 * system_events — the PROJECT-specific event vocabulary for the
 * generic event_manager component (see EventManager_v1.md).
 *
 * Rules: an event states WHAT HAPPENED (never what another module
 * shall do). Payloads are small, copied on publish, and typed by
 * convention per event (documented inline).
 * ============================================================= */

typedef enum {
    EVENT_NONE = 0,

    /* --- System lifecycle (published by src/main) ------------------------- */
    EVT_SYSTEM_BOOT,               /* payload: uint8_t reset_reason            */
    EVT_SYSTEM_READY,              /* all tasks started; no payload            */

    /* --- Network (src/network/wlan_com.c, task_com.c) --------------------- */
    EVT_WLAN_CONNECTED,            /* payload: uint32_t ip4 (network order)    */
    EVT_WLAN_DISCONNECTED,         /* payload: uint8_t reason                  */
    EVT_SESSION_READY,             /* hello_ack accepted; no payload           */
    EVT_SESSION_LOST,              /* no payload                               */

    /* --- OTA (src/network/ota_task.c) -------------------------------------- */
    EVT_OTA_STARTED,               /* payload: char version[16]                */
    EVT_OTA_APPLIED,               /* payload: char version[16]                */
    EVT_OTA_FAILED,                /* payload: evt_ota_failed_t                */
    EVT_OTA_REJECTED_UNSIGNED,     /* signature check refused the image        */

    /* --- Pump (src/device/fountain_controlling.c / later pump_manager) ---- */
    EVT_PUMP_STATE_CHANGED,        /* payload: evt_pump_state_t {old,new}      */
    EVT_PUMP_FAULT,                /* payload: uint8_t fault_code (1=dry run)  */
    EVT_PUMP_FAULT_CLEARED,        /* no payload                               */

    /* --- Configuration / power (task_com, network_config, power_mgmt) ------ */
    EVT_DP_WRITTEN,                /* dp_write applied; payload: uint8_t count */
    EVT_NETWORK_CONFIG_SAVED,      /* Network_Save 1/2; payload: uint8_t action*/
    EVT_NETWORK_CONFIG_RESTORED,   /* Network_Save 3; no payload               */
    EVT_POWER_MODE_CHANGED,        /* payload: uint8_t mode (0=HIGH, 1=LOW)    */

    /* --- TaskManager (work package 2) --------------------------------------*/
    EVT_TASK_STARTED,              /* payload: evt_task_info_t                 */
    EVT_TASK_STOPPED,
    EVT_TASK_CYCLE_OVERRUN,        /* on_cycle exceeded its runtime budget     */
    EVT_TASK_STACK_LOW,

    /* --- Watchdog (work package 4) ------------------------------------------*/
    EVT_WD_WARNING,
    EVT_WD_TIMEOUT,
    EVT_WD_RECOVERY_STARTED,
    EVT_WD_RECOVERY_SUCCESS,
    EVT_WD_RECOVERY_FAILED,
    EVT_WD_REBOOT_REQUESTED,
    EVT_WD_BOOT_DIAGNOSIS,

    /* --- Cross-cutting ------------------------------------------------------*/
    EVT_LOW_MEMORY,
    EVT_FATAL_ERROR,

    /* Monitor cycle completed (5 s). Doubles as the WD_EVENT liveness probe:
     * its subscriber runs in the dispatch task, so a heartbeat from there
     * proves the event pipeline (publish -> queue -> dispatch) is alive. */
    EVT_MONITOR_TICK,

    /* Link-quality hysteresis flipped (Link_Robustness_v1): payload uint8_t
     * poor (1 = POOR). Consumers adapt transmission behaviour (pump_task
     * sample throttling, main's slow-mode/power-save coordination). */
    EVT_LINK_STATE_CHANGED,

    EVENT_MAX
} system_event_t;

/* --- Typed payloads (kept small; EVENT_MANAGER_MAX_PAYLOAD_SIZE bound) ---- */

typedef struct {
    unsigned char ucOld;           /* pump_state_t values                      */
    unsigned char ucNew;
} evt_pump_state_t;

typedef struct {
    unsigned char ucPhase;         /* 0=begin 1=download 2=verify 3=finish     */
    int           slEspErr;        /* esp_err_t of the failing call            */
} evt_ota_failed_t;

typedef struct {
    unsigned char ucTaskId;        /* tm_task_id_t (work package 2)            */
    int           slReason;
} evt_task_info_t;
