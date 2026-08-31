/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "pump_manager.h"

#include <math.h>
#include <string.h>

/* Protocol dp_state values (spec §5.2). */
#define PM_DP_INIT    0
#define PM_DP_OFF     1
#define PM_DP_ON      2
#define PM_DP_AUTO    3
#define PM_DP_MANUAL  4
#define PM_DP_FAULT   5

/* ---------------------------------------------------------------- helpers */

static void enter_state(pm_t *pm, pm_state_t eNew, uint64_t ullNow)
{
    if (pm->state == eNew) return;
    pm->state = eNew;
    pm->state_enter_ms = ullNow;
}

static void latch_fault(pm_t *pm, pm_fault_t eFault, uint64_t ullNow)
{
    bool bWasOn = pm->relay_on;
    pm->fault = eFault;
    pm->relay_on = false;
    if (bWasOn || pm->pump_start_ms > pm->pump_stop_ms)
        pm->pump_stop_ms = ullNow;
    enter_state(pm, PM_STATE_FAULT, ullNow);
}

static void pump_on(pm_t *pm, uint64_t ullNow)
{
    pm->relay_on = true;
    pm->pump_start_ms = ullNow;
    pm->start_bar = pm->m.pressure_filtered_bar;
    pm->m.cycles_total++;
    pm->starts_ms[pm->starts_idx] = ullNow;
    pm->starts_idx = (pm->starts_idx + 1) % PM_START_HISTORY;
}

static void pump_off(pm_t *pm, uint64_t ullNow)
{
    pm->relay_on = false;
    pm->pump_stop_ms = ullNow;
    pm->manual_until_ms = 0;
}

static uint8_t starts_in_last_hour(const pm_t *pm, uint64_t ullNow)
{
    uint8_t ucN = 0;
    for (int i = 0; i < PM_START_HISTORY; i++)
        if (pm->starts_ms[i] && ullNow - pm->starts_ms[i] <= 3600000ULL) ucN++;
    return ucN;
}

static void event_begin(pm_t *pm, uint64_t ullNow)
{
    pm->event_start_ms = ullNow;
    pm->m.event_duration_ms = 0;
    pm->m.event_start_bar = pm->m.pressure_filtered_bar;
    pm->m.event_min_bar = pm->m.pressure_filtered_bar;
    pm->m.event_max_bar = pm->m.pressure_filtered_bar;
    pm->m.plateau_bar = pm->m.pressure_filtered_bar;
    pm->m.est_volume_l_event = 0.0f;
}

static void event_end(pm_t *pm)
{
    pm->event_start_ms = 0;
    pm->m.event_duration_ms = 0;
    pm->m.demand = PM_DEMAND_NONE;
    pm->m.est_flow_l_min = 0.0f;
}

/* Demand classification from the stable-phase plateau (work package §12). */
static pm_demand_t classify_demand(const pm_t *pm)
{
    const pm_config_t *c = &pm->cfg;
    float p = pm->m.plateau_bar;

    if (pm->m.pressure_filtered_bar < c->critical_bar) return PM_DEMAND_PIPE_BREAK;
    if (p >= c->hand_min_bar && p <= c->hand_max_bar)  return PM_DEMAND_HAND;
    if (p >= c->tank_min_bar && p <= c->tank_max_bar)  return PM_DEMAND_TANK;
    return PM_DEMAND_UNKNOWN;
}

/* Explainable anomaly score (thresholds relative to the config). */
static uint16_t anomaly_score(const pm_t *pm)
{
    const pm_config_t *c = &pm->cfg;
    const pm_metrics_t *m = &pm->m;
    uint16_t usScore = 0;

    if (m->pressure_filtered_bar < c->on_bar)          usScore += 5;
    if (m->pressure_filtered_bar < c->critical_bar)    usScore += 50;
    if (m->event_duration_ms > c->unknown_max_ms)      usScore += 20;
    if (m->event_duration_ms > 2 * c->unknown_max_ms)  usScore += 40;
    if (pm->relay_on && m->run_time_s * 1000ULL > (uint64_t)c->max_on_ms / 2)
        usScore += 20;
    if (m->starts_last_hour > c->max_starts_per_hour)  usScore += 30;
    if (m->demand == PM_DEMAND_LEAK_SUSPECT)           usScore += 30;
    return usScore;
}

/* Calibrated flow model Q = k*sqrt(dp); k = 0 -> honest 0 (uncalibrated). */
static float flow_estimate(const pm_t *pm)
{
    float flK;
    switch (pm->m.demand) {
    case PM_DEMAND_HAND: flK = pm->cfg.flow_k_hand; break;
    case PM_DEMAND_TANK: flK = pm->cfg.flow_k_tank; break;
    default: return 0.0f;
    }
    float flDp = pm->cfg.off_bar - pm->m.plateau_bar;
    if (flK <= 0.0f || flDp <= 0.0f) return 0.0f;
    return flK * sqrtf(flDp);
}

/* -------------------------------------------------------------- filtering */

static void filters_update(pm_t *pm, float flBar, uint64_t ullNow)
{
    pm->m.pressure_bar = flBar;

    if (pm->last_update_ms == 0) {
        pm->m.pressure_filtered_bar = flBar;
        pm->last_filtered_bar = flBar;
        pm->m.pressure_slope_bar_s = 0.0f;
        return;
    }
    float flA = pm->cfg.filter_alpha;
    pm->m.pressure_filtered_bar =
        flA * flBar + (1.0f - flA) * pm->m.pressure_filtered_bar;

    float flDtS = (float)(ullNow - pm->last_update_ms) / 1000.0f;
    if (flDtS > 0.001f)
        pm->m.pressure_slope_bar_s =
            (pm->m.pressure_filtered_bar - pm->last_filtered_bar) / flDtS;
    pm->last_filtered_bar = pm->m.pressure_filtered_bar;

    /* Plateau: EMA of the pressure during stable phases (event tracking). */
    if (pm->event_start_ms &&
        fabsf(pm->m.pressure_slope_bar_s) < pm->cfg.stable_slope_bar_s)
        pm->m.plateau_bar = 0.9f * pm->m.plateau_bar +
                            0.1f * pm->m.pressure_filtered_bar;
}

static void event_update(pm_t *pm, uint64_t ullNow)
{
    if (!pm->event_start_ms) return;
    float p = pm->m.pressure_filtered_bar;
    pm->m.event_duration_ms = (uint32_t)(ullNow - pm->event_start_ms);
    if (p < pm->m.event_min_bar) pm->m.event_min_bar = p;
    if (p > pm->m.event_max_bar) pm->m.event_max_bar = p;

    /* Volume integration only while pumping and calibrated. */
    pm->m.est_flow_l_min = flow_estimate(pm);
    if (pm->relay_on && pm->m.est_flow_l_min > 0.0f && pm->last_update_ms) {
        float flDl = pm->m.est_flow_l_min *
                     (float)(ullNow - pm->last_update_ms) / 60000.0f;
        pm->m.est_volume_l_event += flDl;
        pm->m.est_volume_l_total += flDl;
    }
}

/* ------------------------------------------------------------------- API */

void pm_init(pm_t *pm, const pm_config_t *cfg)
{
    memset(pm, 0, sizeof(*pm));
    pm->cfg = *cfg;
    pm->state = PM_STATE_INIT;
    pm->mode = PM_MODE_MANUAL;    /* legacy default: passive until commanded */
}

void pm_config_set(pm_t *pm, const pm_config_t *cfg) { pm->cfg = *cfg; }

void pm_update(pm_t *pm, float flBar, bool bSensorOk, uint64_t ullNow)
{
    const pm_config_t *c = &pm->cfg;

    filters_update(pm, flBar, ullNow);
    event_update(pm, ullNow);

    /* Global protections (any state except an already latched fault). */
    if (pm->state != PM_STATE_FAULT) {
        if (!bSensorOk) {
            pm->m.demand = PM_DEMAND_SENSOR_FAULT;
            latch_fault(pm, PM_FAULT_SENSOR, ullNow);
        } else if (pm->relay_on) {
            uint64_t ullRun = ullNow - pm->pump_start_ms;
            /* Max-runtime fault threshold: with an ACTIVE timed-on (MANUAL
             * with deadline) the manual ceiling applies instead of max_on_ms
             * — the deadline switch-off (timed-off, see RUNNING/MANUAL) then
             * fires BEFORE the fault, even if duration > Fon_Max_On_Time.
             * AUTO and unlimited manual On keep max_on_ms. */
            uint64_t ullMaxRunMs =
                (pm->mode == PM_MODE_MANUAL && pm->manual_until_ms)
                    ? (uint64_t)PM_MANUAL_MAX_ON_S * 1000ULL
                    : c->max_on_ms;
            if (pm->m.pressure_filtered_bar < c->critical_bar &&
                pm->m.event_start_bar >= c->critical_bar) {
                pm->m.demand = PM_DEMAND_PIPE_BREAK;
                latch_fault(pm, PM_FAULT_CRITICAL_LOW, ullNow);
            } else if (ullRun >= ullMaxRunMs) {
                latch_fault(pm, PM_FAULT_MAX_RUNTIME, ullNow);
            } else if (ullRun >= c->dry_detect_ms &&
                       pm->m.pressure_filtered_bar - pm->start_bar <
                           c->dry_min_rise_bar) {
                latch_fault(pm, PM_FAULT_DRY_RUN, ullNow);
            }
        }
    }

    switch (pm->state) {

    case PM_STATE_INIT:
        pm->relay_on = false;
        if (bSensorOk) enter_state(pm, PM_STATE_IDLE, ullNow);
        break;

    case PM_STATE_IDLE:
        pm->relay_on = false;
        if (pm->mode == PM_MODE_AUTO &&
            pm->m.pressure_filtered_bar < c->on_bar) {
            event_begin(pm, ullNow);
            enter_state(pm, PM_STATE_PRESSURE_DROP, ullNow);
        }
        break;

    case PM_STATE_PRESSURE_DROP:
        pm->relay_on = false;
        if (pm->mode != PM_MODE_AUTO) {           /* mode changed mid-drop */
            event_end(pm);
            enter_state(pm, PM_STATE_IDLE, ullNow);
            break;
        }
        if (pm->m.pressure_filtered_bar >= c->on_bar) {
            event_end(pm);                        /* brief dip only        */
            enter_state(pm, PM_STATE_IDLE, ullNow);
            break;
        }
        if (ullNow - pm->state_enter_ms >= c->drop_confirm_ms) {
            pm->m.demand = classify_demand(pm);
            /* PIPE_BREAK only counts as a fault when the system WAS
             * pressurized before the drop — a system that simply starts
             * empty (testbed, first fill) must be allowed to pump up
             * (the dry-run guard still protects the well). */
            if (pm->m.demand == PM_DEMAND_PIPE_BREAK &&
                pm->m.event_start_bar >= c->critical_bar) {
                latch_fault(pm, PM_FAULT_CRITICAL_LOW, ullNow);
            } else {
                enter_state(pm, PM_STATE_STARTING, ullNow);
            }
        }
        break;

    case PM_STATE_STARTING: {
        /* Starts-per-hour guard BEFORE switching on (short-cycle = leak or
         * broken check valve). */
        uint8_t ucStarts = starts_in_last_hour(pm, ullNow);
        if (ucStarts >= c->max_starts_per_hour) {
            latch_fault(pm, PM_FAULT_TOO_MANY_STARTS, ullNow);
            break;
        }
        if (pm->pump_stop_ms == 0 ||
            ullNow - pm->pump_stop_ms >= c->min_off_ms) {
            pump_on(pm, ullNow);
            enter_state(pm, PM_STATE_RUNNING, ullNow);
        } else {
            enter_state(pm, PM_STATE_LOCKOUT, ullNow);   /* wait out min-off */
        }
        break;
    }

    case PM_STATE_RUNNING: {
        pm->relay_on = true;
        uint64_t ullRun = ullNow - pm->pump_start_ms;

        if (pm->mode == PM_MODE_AUTO) {
            pm->m.demand = classify_demand(pm);
            /* Demand running too long -> leak suspicion (kept running until
             * a hard limit hits; the score and log surface it). */
            uint32_t ulLimit =
                pm->m.demand == PM_DEMAND_HAND ? c->hand_max_ms :
                pm->m.demand == PM_DEMAND_TANK ? c->tank_max_ms :
                                                 c->unknown_max_ms;
            if (pm->m.event_duration_ms > ulLimit)
                pm->m.demand = PM_DEMAND_LEAK_SUSPECT;

            if (pm->m.pressure_filtered_bar >= c->off_bar &&
                ullRun >= c->min_on_ms) {
                pump_off(pm, ullNow);
                enter_state(pm, PM_STATE_RECOVERING, ullNow);
            }
        } else {
            /* MANUAL: duration timer (turn_on_duration); pressure does not
             * stop the pump — the global protections above still do. */
            if (pm->manual_until_ms && ullNow >= pm->manual_until_ms &&
                ullRun >= c->min_on_ms) {
                pump_off(pm, ullNow);
                enter_state(pm, PM_STATE_RECOVERING, ullNow);
            }
        }
        break;
    }

    case PM_STATE_RECOVERING:
        pm->relay_on = false;
        /* Stable again (or waited long enough) -> close the event. */
        if ((pm->m.pressure_filtered_bar >= c->on_bar &&
             fabsf(pm->m.pressure_slope_bar_s) < c->stable_slope_bar_s) ||
            ullNow - pm->state_enter_ms >= c->recovery_timeout_ms) {
            event_end(pm);
            enter_state(pm, PM_STATE_LOCKOUT, ullNow);
        }
        break;

    case PM_STATE_LOCKOUT:
        pm->relay_on = false;
        if (pm->pump_stop_ms == 0 ||
            ullNow - pm->pump_stop_ms >= c->min_off_ms)
            enter_state(pm, PM_STATE_IDLE, ullNow);
        break;

    case PM_STATE_FAULT:
        pm->relay_on = false;          /* latched; pm_fault_ack() releases */
        break;
    }

    /* Metrics epilogue. */
    pm->m.starts_last_hour = starts_in_last_hour(pm, ullNow);
    pm->m.anomaly_score = anomaly_score(pm);
    if (pm->relay_on) {
        uint32_t ulRunS = (uint32_t)((ullNow - pm->pump_start_ms) / 1000ULL);
        uint64_t ullCapMs = c->max_on_ms;
        if (pm->mode == PM_MODE_MANUAL && pm->manual_until_ms > pm->pump_start_ms) {
            /* Active timed-on: the remaining time refers to the REQUESTED
             * duration (decoupled from max_on_ms; the duration was already
             * clamped to PM_MANUAL_MAX_ON_S at request time) — otherwise a
             * 10-min timed-on would be capped at Fon_Max_On_Time (5 min). */
            ullCapMs = pm->manual_until_ms - pm->pump_start_ms;
        }
        uint32_t ulCapS = (uint32_t)(ullCapMs / 1000ULL);
        pm->m.run_time_s  = ulRunS;
        pm->m.remaining_s = ulCapS > ulRunS ? ulCapS - ulRunS : 0;
    } else {
        pm->m.run_time_s = 0;
        pm->m.remaining_s = 0;
    }
    pm->last_update_ms = ullNow;
}

/* ---- remote requests ---------------------------------------------------- */

bool pm_request_on(pm_t *pm, uint32_t ulDurationS, uint64_t ullNow)
{
    if (pm->state == PM_STATE_FAULT || pm->state == PM_STATE_INIT) return false;
    /* Legacy rule: never start above the OFF threshold. */
    if (pm->m.pressure_filtered_bar >= pm->cfg.off_bar) return false;
    /* Respect the minimum pause (pump protection) even on manual requests. */
    if (pm->pump_stop_ms && ullNow - pm->pump_stop_ms < pm->cfg.min_off_ms)
        return false;
    if (starts_in_last_hour(pm, ullNow) >= pm->cfg.max_starts_per_hour)
        return false;

    pm->mode = PM_MODE_MANUAL;
    /* Hard-clamp the timed-on to the manual ceiling (15 min); 0 = unlimited
     * (like set_state On) stays unchanged and is subject to max_on_ms. */
    if (ulDurationS > PM_MANUAL_MAX_ON_S) ulDurationS = PM_MANUAL_MAX_ON_S;
    pm->manual_until_ms = ulDurationS ? ullNow + (uint64_t)ulDurationS * 1000ULL : 0;
    if (!pm->event_start_ms) event_begin(pm, ullNow);
    if (!pm->relay_on) pump_on(pm, ullNow);
    enter_state(pm, PM_STATE_RUNNING, ullNow);
    return true;
}

bool pm_request_off(pm_t *pm, uint64_t ullNow)
{
    pm->mode = PM_MODE_MANUAL;
    /* In FAULT the mode is only PRE-SELECTED: the fault stays latched (the
     * relay is already off) so the operator can pick a safe target BEFORE
     * acknowledging — otherwise the ack would drop into IDLE and, in AUTO
     * with low pressure, restart the pump instantly. */
    if (pm->state == PM_STATE_FAULT) return true;
    if (pm->relay_on) {
        /* Minimum on-time still applies: defer the stop to the state
         * machine via a "duration reached" deadline. */
        uint64_t ullMinOffAt = pm->pump_start_ms + pm->cfg.min_on_ms;
        pm->manual_until_ms = ullNow > ullMinOffAt ? ullNow : ullMinOffAt;
    } else {
        enter_state(pm, PM_STATE_IDLE, ullNow);
    }
    return true;
}

bool pm_request_restart(pm_t *pm, uint64_t ullNow)
{
    /* Off -> (min pause) -> on again, in the CURRENT mode. */
    if (pm->state == PM_STATE_FAULT) return false;
    if (pm->relay_on) {
        pump_off(pm, ullNow);
        enter_state(pm, PM_STATE_RECOVERING, ullNow);
    }
    return true;
}

bool pm_mode_set(pm_t *pm, pm_mode_t eMode, uint64_t ullNow)
{
    /* Allowed in every state incl. FAULT: the mode is only a preference. In
     * FAULT the relay is off and the fault stays latched (state untouched),
     * so pre-selecting Manual here lets a later ack drop into IDLE WITHOUT an
     * unintended restart. Only the relay-side effect is gated on !FAULT. */
    pm->mode = eMode;
    if (pm->state != PM_STATE_FAULT && eMode == PM_MODE_MANUAL && pm->relay_on)
        pm->manual_until_ms = 0;          /* keep running until told off  */
    (void)ullNow;
    return true;
}

bool pm_fault_ack(pm_t *pm, bool bSensorOk, uint64_t ullNow)
{
    if (pm->state != PM_STATE_FAULT) return false;
    if (!bSensorOk) return false;
    pm->fault = PM_FAULT_NONE;
    event_end(pm);
    pm->m.demand = PM_DEMAND_NONE;
    enter_state(pm, PM_STATE_IDLE, ullNow);
    return true;
}

/* ---- outputs ------------------------------------------------------------ */

bool pm_relay_get(const pm_t *pm) { return pm->relay_on; }

uint8_t pm_dp_state_get(const pm_t *pm)
{
    switch (pm->state) {
    case PM_STATE_INIT:     return PM_DP_INIT;
    case PM_STATE_FAULT:    return PM_DP_FAULT;
    case PM_STATE_STARTING:
    case PM_STATE_RUNNING:  return pm->relay_on ? PM_DP_ON :
                                   (pm->mode == PM_MODE_AUTO ? PM_DP_AUTO
                                                             : PM_DP_OFF);
    default:
        return pm->mode == PM_MODE_AUTO ? PM_DP_AUTO : PM_DP_OFF;
    }
}

bool pm_idle_get(const pm_t *pm)
{
    return !pm->relay_on &&
           (pm->state == PM_STATE_IDLE || pm->state == PM_STATE_LOCKOUT ||
            pm->state == PM_STATE_FAULT);
}
