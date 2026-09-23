/*
 * Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 *
 * This source code is proprietary. No license is granted to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of
 * this software without prior written permission from the copyright holder.
 *
 * For licensing inquiries, contact: info@melowsyne.com
 */

#include "pump_task.h"

#include "hal.h"
#include "data_store.h"
#include "datapoints.h"
#include "event_manager.h"
#include "logging.h"
#include "pressure_history.h"
#include "debug.h"
#include "fountain_proto.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <math.h>

#define TAG "pump"

/* Sensor health: conversion must succeed AND the 0.5-V-based curve needs a
 * live signal (< 300 mV = broken wire/sensor; testbed idles at ~500 mV). */
#define PUMP_SENSOR_MIN_MV   300
/* Debounce: a SINGLE I2C glitch must not latch a sensor fault (it would
 * stop the plant until a manual ack) — only N consecutive bad readings
 * count as a broken sensor; single misses reuse the last good value. */
#define PUMP_SENSOR_FAIL_LIMIT 3
/* Plausibility: a >5-bar jump within one 200-ms cycle is physically
 * impossible for this plant — treat it as a bad reading (electrical
 * disturbance, e.g. pump inrush), counted into the debounce above. */
#define PUMP_MAX_JUMP_BAR      5.0f
/* Re-sync (bring-up 2026-09-21): the jump filter must not get stuck
 * permanently when the pressure has REALLY jumped to a new level (sensor
 * swap, fast plant transient, testbed). If PUMP_JUMP_RESYNC_N consecutive
 * rejected readings lie consistently (± PUMP_JUMP_CONSIST_BAR) on the same
 * new level, it is adopted as the new reality; a single glitch is still
 * kept out. */
#define PUMP_JUMP_RESYNC_N     5      /* ~1 s at 200-ms cycle   */
#define PUMP_JUMP_CONSIST_BAR  1.0f

/* Consistent view of the pm for everything OUTSIDE the lock (mirroring,
 * events, alerts, probes): taken under the lock once per cycle. Probes read
 * single fields (written only by this task -> 32-bit reads are atomic);
 * the snapshot lags a request by at most one 200-ms cycle. */
typedef struct {
    pm_state_t   eState;
    pm_mode_t    eMode;
    pm_fault_t   eFault;
    bool         bRelay;
    bool         bIdle;
    uint8_t      ucDpState;
    pm_metrics_t stM;
} pump_snap_t;

static pm_t s_stPm;
static SemaphoreHandle_t s_hLock;      /* cycle vs. remote requests   */
static pump_snap_t s_stSnap;           /* last cycle's consistent view */
static uint32_t s_ulCyclesPerCfg;      /* config refresh divider      */
static uint64_t s_ullLastSampleMs;     /* 1-Hz time-series divider    */
static uint64_t s_ullLastHistMs;       /* 1-Hz pressure-history divider */
static pm_metrics_t s_stPrevM;         /* metrics before this update  */
static pm_state_t s_ePrevState = PM_STATE_INIT;
static pm_fault_t s_ePrevFault = PM_FAULT_NONE;
static bool s_bOverPressure;           /* edge for the high alert     */
static bool s_bRelayFailed;            /* edge for the actuator error */
static uint8_t s_ucPrevLabel;
static uint8_t s_ucSensorFails;        /* consecutive bad readings    */
static float s_flJumpPendingBar;       /* anchor of the re-sync candidate */
static uint8_t s_ucJumpPending;        /* consistent rejected sequence */
static uint32_t s_ulSensorErrTotal;    /* cumulative, mirrors to a DP */
static float s_flLastGoodBar;          /* held during single misses   */
static bool s_bHaveGood;

/* Offline recorder / poor-link throttling (Link_Robustness_v1 §B1/B2):
 * with no session the time series is raised from DEBUG to INFO so the funk-
 * hole history lands in the log ring and gets backfilled via since_seq;
 * on a POOR (but connected) link the sample rate is halved instead. */
static volatile bool s_bSessionUp;
static volatile bool s_bLinkPoor;

static void pump_on_net_event(system_event_t eEvent, const void *pvData,
                              size_t szSize)
{
    switch (eEvent) {
    case EVT_SESSION_READY:
        s_bSessionUp = true;
        /* A fault latched inside a funk hole must ALERT, not just sit in
         * the log: re-emit the pending fault once the session is back. */
        if (s_stSnap.eFault != PM_FAULT_NONE)
            fountain_proto_alert_send(
                s_stSnap.eFault == PM_FAULT_DRY_RUN ? "dry_run" : "pump_fault",
                "critical", "Fon_Fault_Code", (double)s_stSnap.eFault, 0.0,
                "pump fault latched (re-emit after reconnect)");
        break;
    case EVT_SESSION_LOST:
        s_bSessionUp = false;
        break;
    case EVT_LINK_STATE_CHANGED:
        if (pvData && szSize >= 1)
            s_bLinkPoor = (*(const uint8_t *)pvData) != 0;
        break;
    default:
        break;
    }
}

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000ULL); }

/* Map the Fon_* datapoints into the pure config (times arrive in s/min/mbar). */
static void config_from_dps(pm_config_t *c)
{
    dp_lock(portMAX_DELAY);
    c->on_bar             = DP_REF(Fon_Min_Pressure);
    c->off_bar            = DP_REF(Fon_Max_Pressure);
    c->critical_bar       = DP_REF(Fon_Alert_Low_Pressure);
    c->hand_min_bar       = DP_REF(Fon_Hand_Min_Pressure);
    c->hand_max_bar       = DP_REF(Fon_Hand_Max_Pressure);
    c->tank_min_bar       = DP_REF(Fon_Tank_Min_Pressure);
    c->tank_max_bar       = DP_REF(Fon_Tank_Max_Pressure);
    c->filter_alpha       = DP_REF(Fon_Filter_Alpha);
    c->stable_slope_bar_s = DP_REF(Fon_Stable_Slope);
    c->dry_min_rise_bar   = (float)DP_REF(Fon_Dry_Run_Min_Rise) / 1000.0f;
    c->flow_k_hand        = DP_REF(Fon_Flow_K_Hand);
    c->flow_k_tank        = DP_REF(Fon_Flow_K_Tank);
    c->drop_confirm_ms    = (uint32_t)DP_REF(Fon_Pressure_Drop_Rate);
    c->min_on_ms          = (uint32_t)DP_REF(Fon_Min_On_Time)  * 1000u;
    c->min_off_ms         = (uint32_t)DP_REF(Fon_Min_Off_Time) * 1000u;
    c->max_on_ms          = (uint32_t)DP_REF(Fon_Max_On_Time)  * 1000u;
    c->dry_detect_ms      = (uint32_t)DP_REF(Fon_Dry_Run_Detect_Time) * 1000u;
    c->recovery_timeout_ms= (uint32_t)DP_REF(Fon_Check_Valve_Timeout) * 1000u;
    c->hand_max_ms        = (uint32_t)DP_REF(Fon_Hand_Max_Duration)    * 60000u;
    c->tank_max_ms        = (uint32_t)DP_REF(Fon_Tank_Max_Duration)    * 60000u;
    c->unknown_max_ms     = (uint32_t)DP_REF(Fon_Unknown_Max_Duration) * 60000u;
    c->max_starts_per_hour= DP_REF(Fon_Max_Starts_Per_Hour);

    hal_pressure_calibration_set(DP_REF(Fon_Sensor_Range_Bar),
                                 DP_REF(Fon_Sensor_Scale),
                                 (int16_t)DP_REF(Fon_Sensor_Offset));
    dp_unlock();
}

bool pump_task_init(void)
{
    s_hLock = xSemaphoreCreateMutex();
    if (!s_hLock) return false;
    event_manager_subscribe(EVT_SESSION_READY,      pump_on_net_event);
    event_manager_subscribe(EVT_SESSION_LOST,       pump_on_net_event);
    event_manager_subscribe(EVT_LINK_STATE_CHANGED, pump_on_net_event);
    pm_config_t stCfg;
    config_from_dps(&stCfg);
    pm_init(&s_stPm, &stCfg);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "init (range=%.1f bar, on<%.2f off>%.2f)", stCfg.on_bar >= 0 ?
            DP_REF(Fon_Sensor_Range_Bar) : 0, stCfg.on_bar, stCfg.off_bar);
    return true;
}

/* Publish transitions + write the log records that need edge context.
 * Operates on the LOCKED snapshot only (never on the live pm). */
static void report_edges(const pump_snap_t *pstSnap)
{
    if (pstSnap->eState != s_ePrevState) {
        evt_pump_state_t stEv = { .ucOld = (unsigned char)s_ePrevState,
                                  .ucNew = (unsigned char)pstSnap->eState };
        event_manager_publish(EVT_PUMP_STATE_CHANGED, &stEv, sizeof(stEv));
    }
    if (pstSnap->eFault != s_ePrevFault) {
        if (pstSnap->eFault != PM_FAULT_NONE) {
            uint8_t ucCode = (uint8_t)pstSnap->eFault;
            event_manager_publish(EVT_PUMP_FAULT, &ucCode, sizeof(ucCode));
            /* dry_run keeps its established alert name; everything else
             * surfaces as pump_fault with the code. */
            fountain_proto_alert_send(
                pstSnap->eFault == PM_FAULT_DRY_RUN ? "dry_run" : "pump_fault",
                "critical", "Fon_Fault_Code", (double)pstSnap->eFault, 0.0,
                "pump fault latched");
        } else {
            event_manager_publish(EVT_PUMP_FAULT_CLEARED, NULL, 0);
        }
    }
    /* Event summary: the pure layer closed an event this cycle. */
    if (s_stPrevM.event_duration_ms && !pstSnap->stM.event_duration_ms &&
        s_stPrevM.event_duration_ms > 2000) {
        LOG_EMIT4(LOG_LEVEL_INFO, LOG_MOD_PUMP, LOG_EVT_PM_EVENT,
                  s_stPrevM.event_duration_ms / 1000,
                  (int32_t)(s_stPrevM.event_min_bar * 1000.0f),
                  s_stPrevM.demand,
                  (int32_t)(s_stPrevM.est_volume_l_event * 10.0f),
                  "pm event summary");
    }
    s_ePrevState = pstSnap->eState;
    s_ePrevFault = pstSnap->eFault;
}

static void mirror_dps(const pump_snap_t *pstSnap, uint32_t ulMv, bool bMeasOk)
{
    const pm_metrics_t *m = &pstSnap->stM;
    dp_lock(portMAX_DELAY);
    DP_REF(Fon_Current_Pressure)  = m->pressure_bar;
    if (bMeasOk)                           /* keep the last real reading */
        DP_REF(Fon_Sensor_Voltage_mV) = ulMv;
    DP_REF(Fon_Current_State)     = pstSnap->ucDpState;
    DP_REF(Fon_Relay_Output)      = pstSnap->bRelay ? 1 : 0;
    DP_REF(Fon_Run_Time)          = m->run_time_s;
    DP_REF(Fon_Remaining_Time)    = m->remaining_s;
    DP_REF(Fon_Cycles_Total)      = m->cycles_total;
    DP_REF(Fon_Pressure_Filtered) = m->pressure_filtered_bar;
    DP_REF(Fon_Pressure_Slope)    = m->pressure_slope_bar_s;
    DP_REF(Fon_Demand_State)      = (uint8_t)m->demand;
    DP_REF(Fon_Anomaly_Score)     = m->anomaly_score;
    DP_REF(Fon_Event_Duration)    = m->event_duration_ms / 1000;
    DP_REF(Fon_Est_Flow_L_Min)    = m->est_flow_l_min;
    DP_REF(Fon_Est_Volume_Total)  = m->est_volume_l_total;
    DP_REF(Fon_Fault_Code)        = (uint8_t)pstSnap->eFault;
    DP_REF(Fon_Starts_Per_Hour)   = m->starts_last_hour;
    DP_REF(Fon_Sensor_Err_Count)  = s_ulSensorErrTotal;
    DP_REF(Fon_Sensor_Noise_mV)   = hal_pressure_noise_mv_get();
    dp_unlock();
}

/* Volatile command/label points, polled per cycle. */
static void poll_command_dps(uint64_t ullNow, bool bSensorOk)
{
    uint8_t ucAck, ucLabel, ucCmd;
    dp_lock(portMAX_DELAY);
    ucAck   = DP_REF(Fon_Fault_Ack);
    ucLabel = DP_REF(Fon_Event_Label);
    ucCmd   = DP_REF(Fon_Pump_Command);
    if (ucAck) DP_REF(Fon_Fault_Ack) = 0;
    if (ucCmd) DP_REF(Fon_Pump_Command) = 0;
    dp_unlock();

    if (ucCmd) {
        /* Same semantics as the protocol set_state/restart commands; the
         * pump manager applies its own rules (fault latch, min-off, ...). */
        bool bOk = false;
        switch (ucCmd) {
        case 1: bOk = pm_request_on(&s_stPm, 0, ullNow);              break;
        case 2: bOk = pm_request_off(&s_stPm, ullNow);                break;
        case 3: bOk = pm_mode_set(&s_stPm, PM_MODE_AUTO, ullNow);     break;
        case 4: bOk = pm_mode_set(&s_stPm, PM_MODE_MANUAL, ullNow);   break;
        case 5: bOk = pm_request_restart(&s_stPm, ullNow);            break;
        default: break;
        }
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "Fon_Pump_Command %u -> %s",
                ucCmd, bOk ? "accepted" : "refused");
    }

    if (ucAck) {
        bool bOk = pm_fault_ack(&s_stPm, bSensorOk, ullNow);
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "fault ack -> %s",
                bOk ? "cleared" : "refused");
    }
    if (ucLabel != s_ucPrevLabel) {
        s_ucPrevLabel = ucLabel;
        LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_PUMP, LOG_EVT_PM_LABEL,
                  ucLabel, 0, "manual event label");
    }
}

esp_err_t pump_task_cycle(tm_task_ctx_t *pstCtx)
{
    (void)pstCtx;
    uint64_t ullNow = now_ms();

    /* Config + sensor curve refresh once per second (cheap DP reads). */
    if (++s_ulCyclesPerCfg >= 1000 / PUMP_TASK_PERIOD_MS) {
        s_ulCyclesPerCfg = 0;
        pm_config_t stCfg;
        config_from_dps(&stCfg);
        xSemaphoreTake(s_hLock, portMAX_DELAY);
        pm_config_set(&s_stPm, &stCfg);
        xSemaphoreGive(s_hLock);
    }

    float flBar = 0.0f;
    uint32_t ulMv = 0;
    bool bReadOk = hal_pressure_read(&ulMv, &flBar);
    bool bMeasOk = bReadOk && ulMv >= PUMP_SENSOR_MIN_MV;

    /* Pressure simulation override (Fon_Pressure_Manual): feed Fon_Pressure_Value
     * into the control chain instead of the sensor — run pressure scenarios
     * without the pump. The simulated reading is always valid and bypasses the
     * jump filter (its steps are intentional, not glitches). The hardware
     * voltage is still read and mirrored, so the real sensor stays observable. */
    bool bManual = DP_REF(Fon_Pressure_Manual) != 0;
    if (bManual) {
        flBar   = DP_REF(Fon_Pressure_Value);
        bMeasOk = true;
    }

    /* Jump filter (plan 5.2): physically impossible steps are glitches.
     * With re-sync: a consistent sequence on a new level is adopted as a
     * real level change after PUMP_JUMP_RESYNC_N samples. */
    if (!bManual && bMeasOk && s_bHaveGood &&
        fabsf(flBar - s_flLastGoodBar) > PUMP_MAX_JUMP_BAR) {
        if (s_ucJumpPending &&
            fabsf(flBar - s_flJumpPendingBar) <= PUMP_JUMP_CONSIST_BAR) {
            s_ucJumpPending++;
        } else {
            s_flJumpPendingBar = flBar;       /* new candidate/anchor */
            s_ucJumpPending = 1;
        }
        if (s_ucJumpPending >= PUMP_JUMP_RESYNC_N) {
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "pressure level re-sync %.2f -> %.2f bar "
                    "(%u consistent samples)",
                    s_flLastGoodBar, flBar, (unsigned)s_ucJumpPending);
            s_ucJumpPending = 0;              /* sample stays valid   */
        } else {
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "implausible pressure jump %.2f -> %.2f bar — sample dropped",
                    s_flLastGoodBar, flBar);
            bMeasOk = false;
        }
    } else {
        s_ucJumpPending = 0;   /* accepted/invalid sample breaks the sequence */
    }

    /* Debounce (plan 3.1): a single glitch neither latches a fault nor
     * feeds garbage into the filters — hold the last good value; only
     * PUMP_SENSOR_FAIL_LIMIT consecutive misses report a broken sensor. */
    if (bMeasOk) {
        s_ucSensorFails = 0;
        s_flLastGoodBar = flBar;
        s_bHaveGood = true;
    } else {
        if (s_ucSensorFails < 255) s_ucSensorFails++;
        s_ulSensorErrTotal++;
        logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG,
                "pressure read failed (%u consecutive)", s_ucSensorFails);
    }
    bool  bSensorOk = bMeasOk || s_ucSensorFails < PUMP_SENSOR_FAIL_LIMIT;
    float flUseBar  = bMeasOk ? flBar : (s_bHaveGood ? s_flLastGoodBar : 0.0f);

    /* 1-Hz pressure history (drucksensor_datenstruktur.md): EXACTLY one
     * sample per second, even on sensor error — the status documents the
     * quality. Separate from the live datapoint (draft §21) and PM_SAMPLE. */
    if (ullNow - s_ullLastHistMs >= PRESSURE_HISTORY_INTERVAL_MS) {
        s_ullLastHistMs = ullNow;
        uint16_t usHistStatus;
        float    flHistBar;
        if (bManual) {
            flHistBar    = flBar;               /* = Fon_Pressure_Value */
            usHistStatus = PRESSURE_STATUS_VALID | PRESSURE_STATUS_MANUAL;
        } else if (bMeasOk) {
            flHistBar    = flBar;
            usHistStatus = PRESSURE_STATUS_VALID;
        } else {
            flHistBar    = s_bHaveGood ? s_flLastGoodBar : 0.0f;
            usHistStatus = PRESSURE_STATUS_SENSOR_ERROR |
                           (s_bHaveGood ? PRESSURE_STATUS_STALE : 0u);
        }
        if (flHistBar < 0.0f) {
            flHistBar = 0.0f;
            usHistStatus |= PRESSURE_STATUS_OUT_OF_RANGE;
        } else if (flHistBar > 65.535f) {
            flHistBar = 65.535f;
            usHistStatus |= PRESSURE_STATUS_OUT_OF_RANGE;
        }
        pressure_history_add((uint16_t)lroundf(flHistBar * 1000.0f), usHistStatus);
    }

    pump_snap_t stSnap;
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    /* Fetch the timestamp ONLY under the lock: ADC measurement/filters sit
     * between the cycle start and here; a remote request (pump_request_on)
     * in this window carries a newer now_ms() than the old ullNow
     * -> pm_update would see a negative run time (see run_ms in pump_manager). */
    ullNow = now_ms();
    s_stPrevM = s_stSnap.stM;
    pm_update(&s_stPm, flUseBar, bSensorOk, ullNow);
    poll_command_dps(ullNow, bSensorOk);
    /* Consistent view for everything below + for the lock-free probes. */
    stSnap.eState    = s_stPm.state;
    stSnap.eMode     = s_stPm.mode;
    stSnap.eFault    = s_stPm.fault;
    stSnap.bRelay    = pm_relay_get(&s_stPm);
    stSnap.bIdle     = pm_idle_get(&s_stPm);
    stSnap.ucDpState = pm_dp_state_get(&s_stPm);
    stSnap.stM       = s_stPm.m;
    s_stSnap = stSnap;
    xSemaphoreGive(s_hLock);

    /* Actuator check (plan 3.2): a failing relay write is an ERROR —
     * edge-triggered record + local log; recovery clears the edge. */
    if (!hal_relay_set(stSnap.bRelay)) {
        if (!s_bRelayFailed) {
            s_bRelayFailed = true;
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "ERROR: relay write failed (want %d)", (int)stSnap.bRelay);
            LOG_EMIT2(LOG_LEVEL_ERROR, LOG_MOD_PUMP, LOG_EVT_PM_RELAY_FAIL,
                      stSnap.bRelay, 0, "relay write failed");
        }
    } else if (s_bRelayFailed) {
        s_bRelayFailed = false;
        LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_PUMP, LOG_EVT_PM_RELAY_FAIL,
                  stSnap.bRelay, 1, "relay write recovered");
    }

    if (bMeasOk) data_store_float_set(DATA_PRESSURE, flBar);
    mirror_dps(&stSnap, ulMv, bMeasOk);
    report_edges(&stSnap);

    /* Over-pressure alert (edge-triggered, established name/severity). */
    float flHigh = DP_REF(Fon_Alert_High_Pressure);
    if (bSensorOk && flHigh > 0.0f) {
        if (stSnap.stM.pressure_filtered_bar > flHigh && !s_bOverPressure) {
            s_bOverPressure = true;
            fountain_proto_alert_send("over_pressure", "warning",
                                      "Fon_Current_Pressure",
                                      stSnap.stM.pressure_filtered_bar, flHigh,
                                      "pressure above alert threshold");
        } else if (stSnap.stM.pressure_filtered_bar < flHigh - 0.2f) {
            s_bOverPressure = false;
        }
    }

    /* Structured time series. Level/rate adapt to the link (§B1/B2):
     *   session up + link GOOD: DEBUG (opt-in via Log_Runtime_Level)
     *   session up + link POOR: DEBUG at half rate
     *   NO session (funk hole): INFO at >=30 s — the OFFLINE RECORDER:
     *   the history lands in the ring and is backfilled after reconnect. */
    uint32_t ulSampleS = DP_REF(Fon_Report_Interval);
    if (ulSampleS == 0) ulSampleS = 1;
    log_level_t eSampleLvl = LOG_LEVEL_DEBUG;
    if (!s_bSessionUp) {
        eSampleLvl = LOG_LEVEL_INFO;
        if (ulSampleS < 30) ulSampleS = 30;
    } else if (s_bLinkPoor) {
        ulSampleS *= 2;
    }
    if (ullNow - s_ullLastSampleMs >= (uint64_t)ulSampleS * 1000ULL) {
        s_ullLastSampleMs = ullNow;
        LOG_EMIT4(eSampleLvl, LOG_MOD_PUMP, LOG_EVT_PM_SAMPLE,
                  (int32_t)(stSnap.stM.pressure_filtered_bar * 1000.0f),
                  (int32_t)(stSnap.stM.pressure_slope_bar_s * 1000.0f),
                  ((int32_t)stSnap.eState << 8) | (int32_t)stSnap.stM.demand,
                  stSnap.stM.anomaly_score, NULL);
    }
    return ESP_OK;
}

/* ---- remote requests (WS/command context; serialized via the lock) ------ */

bool pump_request_on(uint32_t ulMaxDurationS)
{
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    bool bOk = pm_request_on(&s_stPm, ulMaxDurationS, now_ms());
    xSemaphoreGive(s_hLock);
    return bOk;
}

bool pump_request_off(void)
{
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    bool bOk = pm_request_off(&s_stPm, now_ms());
    xSemaphoreGive(s_hLock);
    return bOk;
}

bool pump_request_restart(void)
{
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    bool bOk = pm_request_restart(&s_stPm, now_ms());
    xSemaphoreGive(s_hLock);
    return bOk;
}

bool pump_mode_set(pm_mode_t eMode)
{
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    bool bOk = pm_mode_set(&s_stPm, eMode, now_ms());
    xSemaphoreGive(s_hLock);
    return bOk;
}

/* Lock-free probes: single fields of the cycle snapshot (32-bit reads are
 * atomic; written only by the pump task, at most one cycle stale). */
pm_state_t pump_state_get(void) { return s_stSnap.eState; }

bool pump_idle_get(void) { return s_stSnap.bIdle; }
