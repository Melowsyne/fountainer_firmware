/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================
 * pump_manager — PURE pressure-based pump state machine
 * (PumpManager_v1.md, work package 5; replaces the former
 * fountain_controlling module).
 *
 * PURE: no I/O, no FreeRTOS, no datapoints — inputs (pressure,
 * sensor health, time) come in via pm_update(), the relay
 * decision and all metrics come out of the struct. The hardware/
 * datapoint binding lives in pump_task.c; host tests drive this
 * file directly.
 *
 * Semantics kept from the legacy module (proven in operation):
 * AUTO/MANUAL remote modes (set_state On/Off/Auto/Manual,
 * turn_on_duration), minimum on/off times, no start above the
 * off threshold, DRY-RUN protection, faults latch until an
 * explicit acknowledge. New per the work package: confirmed
 * pressure-drop entry, RECOVERING/LOCKOUT cycle, demand
 * classification (hand valve / tank / leak suspect / pipe
 * break), starts-per-hour guard, EMA + slope + plateau, event
 * metrics, anomaly score, calibrated flow/volume estimate
 * (k=0 -> reported as 0; no orifice guessing).
 *
 * Simplifications vs. the paper (documented deliberately):
 *  - closed_min == the ON threshold (no separate datapoint)
 *  - RECOVERING times out into LOCKOUT (no fault; persistent
 *    demand simply re-triggers the AUTO cycle; leaks surface
 *    via duration limits and the anomaly score)
 *  - no MAINTENANCE state (no local service switch exists);
 *    remote MANUAL covers controlled operation
 *  - fault acknowledge requires a healthy sensor only (NOT a
 *    pressurized system — the testbed sits at 0 bar)
 * ============================================================= */

typedef enum {
    PM_STATE_INIT = 0,
    PM_STATE_IDLE,           /* AUTO: armed / MANUAL: off               */
    PM_STATE_PRESSURE_DROP,  /* below ON threshold, awaiting confirm    */
    PM_STATE_STARTING,       /* start requested, min-off gate           */
    PM_STATE_RUNNING,
    PM_STATE_RECOVERING,     /* pump off, waiting for stable pressure   */
    PM_STATE_LOCKOUT,        /* minimum-off pause                       */
    PM_STATE_FAULT,          /* latched; needs pm_fault_ack             */
} pm_state_t;

typedef enum { PM_MODE_MANUAL = 0, PM_MODE_AUTO } pm_mode_t;

typedef enum {
    PM_DEMAND_NONE = 0,
    PM_DEMAND_UNKNOWN,
    PM_DEMAND_HAND,          /* hand-watering valve plateau band        */
    PM_DEMAND_TANK,          /* storage-tank valve plateau band         */
    PM_DEMAND_LEAK_SUSPECT,  /* classified demand for too long          */
    PM_DEMAND_PIPE_BREAK,    /* below the critical threshold            */
    PM_DEMAND_SENSOR_FAULT,
} pm_demand_t;

typedef enum {
    PM_FAULT_NONE = 0,
    PM_FAULT_SENSOR,
    PM_FAULT_CRITICAL_LOW,   /* pressure below critical while running   */
    PM_FAULT_MAX_RUNTIME,
    PM_FAULT_DRY_RUN,        /* no pressure rise after start (well!)    */
    PM_FAULT_TOO_MANY_STARTS,
} pm_fault_t;

/* All thresholds come from the Fon_* datapoints (pump_task maps them);
 * defaults preserve today's live values. */
typedef struct {
    float on_bar;                 /* Fon_Min_Pressure  (start below)      */
    float off_bar;                /* Fon_Max_Pressure  (stop above)       */
    float critical_bar;           /* Fon_Alert_Low_Pressure               */
    float hand_min_bar, hand_max_bar;   /* demand plateau bands           */
    float tank_min_bar, tank_max_bar;
    float filter_alpha;           /* EMA coefficient                      */
    float stable_slope_bar_s;     /* |slope| below this = stable          */
    float dry_min_rise_bar;       /* Fon_Dry_Run_Min_Rise (mbar -> bar)   */
    float flow_k_hand, flow_k_tank;  /* Q = k*sqrt(dp); 0 = uncalibrated  */
    uint32_t drop_confirm_ms;     /* Fon_Pressure_Drop_Rate (ms)          */
    uint32_t min_on_ms, min_off_ms, max_on_ms;
    uint32_t dry_detect_ms;       /* Fon_Dry_Run_Detect_Time              */
    uint32_t recovery_timeout_ms; /* Fon_Check_Valve_Timeout              */
    uint32_t hand_max_ms, tank_max_ms, unknown_max_ms;  /* -> LEAK_SUSPECT */
    uint8_t  max_starts_per_hour;
} pm_config_t;

#define PM_START_HISTORY 16       /* start timestamps for the hourly guard */

/* Upper bound for TIME-LIMITED manual switch-on (turn_on_duration).
 * An active timed-on is decoupled from the Fon_Max_On_Time fault limit:
 * it switches off cleanly at its deadline (no MAX_RUNTIME fault) but is
 * hard-clamped to this ceiling. Fon_Max_On_Time remains the safety
 * limit for AUTO and for unlimited manual On. */
#define PM_MANUAL_MAX_ON_S 900u   /* 15 min */

typedef struct {
    float pressure_bar;           /* raw input of the last update         */
    float pressure_filtered_bar;
    float pressure_slope_bar_s;
    float plateau_bar;            /* EMA of stable-phase pressure         */
    pm_demand_t demand;
    uint16_t anomaly_score;
    uint32_t event_duration_ms;
    float event_start_bar, event_min_bar, event_max_bar;
    float est_flow_l_min;
    float est_volume_l_event, est_volume_l_total;
    uint32_t run_time_s;          /* current run (0 when off)             */
    uint32_t remaining_s;         /* until max runtime / manual duration  */
    uint32_t cycles_total;        /* pump starts since boot               */
    uint8_t  starts_last_hour;
} pm_metrics_t;

typedef struct {
    pm_config_t cfg;
    pm_state_t  state;
    pm_mode_t   mode;
    pm_fault_t  fault;            /* latched                              */
    bool relay_on;

    uint64_t state_enter_ms, last_update_ms;
    uint64_t pump_start_ms, pump_stop_ms, event_start_ms;
    uint64_t manual_until_ms;     /* turn_on_duration deadline (0 = none) */
    float    start_bar;           /* pressure at pump start (dry run)     */
    float    last_filtered_bar;
    uint64_t starts_ms[PM_START_HISTORY];
    uint8_t  starts_idx;

    pm_metrics_t m;
} pm_t;

void pm_init(pm_t *pm, const pm_config_t *cfg);
void pm_config_set(pm_t *pm, const pm_config_t *cfg);

/* One control tick (pure): fresh raw pressure + sensor health + time. */
void pm_update(pm_t *pm, float pressure_bar, bool sensor_ok, uint64_t now_ms);

/* Remote requests (command module / protocol). Return false if refused. */
bool pm_request_on(pm_t *pm, uint32_t duration_s, uint64_t now_ms);
bool pm_request_off(pm_t *pm, uint64_t now_ms);
bool pm_request_restart(pm_t *pm, uint64_t now_ms);   /* off -> on cycle  */
bool pm_mode_set(pm_t *pm, pm_mode_t mode, uint64_t now_ms);
bool pm_fault_ack(pm_t *pm, bool sensor_ok, uint64_t now_ms);

/* Outputs. */
bool     pm_relay_get(const pm_t *pm);
uint8_t  pm_dp_state_get(const pm_t *pm);   /* protocol dp_state 0..5     */
bool     pm_idle_get(const pm_t *pm);       /* power_mgmt probe           */

#ifdef __cplusplus
}
#endif
