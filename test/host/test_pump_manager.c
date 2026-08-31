/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* Host tests of the PURE pump_manager state machine (work package 5) —
 * no mocks needed: pressure/sensor/time go in, relay/state/metrics come
 * out. Covers the legacy guarantees (min on/off, dry run, no start above
 * OFF, AUTO hysteresis, restart/max runtime, mode mapping) plus the new
 * behaviour (drop confirm, recovering/lockout, demand classification,
 * leak suspicion, starts-per-hour guard, fault ack, dp_state mapping). */

#include <stdio.h>
#include "pump_manager.h"

static int s_slFails;
#define CHECK(cond, name)                                             \
    do { if (cond) { printf("PASS %s\n", name); }                     \
         else { printf("FAIL %s (line %d)\n", name, __LINE__);        \
                s_slFails++; } } while (0)

/* Testbed-like config: fast times so scenarios stay compact. */
static pm_config_t cfg_default(void)
{
    pm_config_t c = {
        .on_bar = 2.0f, .off_bar = 3.5f, .critical_bar = 0.3f,
        .hand_min_bar = 1.7f, .hand_max_bar = 2.0f,
        .tank_min_bar = 1.2f, .tank_max_bar = 1.6f,
        .filter_alpha = 1.0f,          /* no smoothing: direct values     */
        .stable_slope_bar_s = 0.05f,
        .dry_min_rise_bar = 0.1f,
        .flow_k_hand = 0.0f, .flow_k_tank = 0.0f,
        .drop_confirm_ms = 3000,
        .min_on_ms = 5000, .min_off_ms = 4000, .max_on_ms = 60000,
        .dry_detect_ms = 10000,
        .recovery_timeout_ms = 10000,
        .hand_max_ms = 60000, .tank_max_ms = 60000, .unknown_max_ms = 20000,
        .max_starts_per_hour = 5,
    };
    return c;
}

/* Advance n ticks of dt_ms with constant pressure. */
static void run(pm_t *pm, uint64_t *now, float bar, int n, int dt_ms)
{
    for (int i = 0; i < n; i++) {
        *now += (uint64_t)dt_ms;
        pm_update(pm, bar, true, *now);
    }
}

static void test_boot_and_manual_dry_run(void)
{
    pm_config_t c = cfg_default();
    pm_t pm;
    uint64_t now = 1000;
    pm_init(&pm, &c);

    pm_update(&pm, 0.05f, true, now);
    CHECK(pm.state == PM_STATE_IDLE && !pm_relay_get(&pm),
          "boot: INIT -> IDLE, relay off");
    CHECK(pm.mode == PM_MODE_MANUAL && pm_dp_state_get(&pm) == 1,
          "boot: legacy default MANUAL -> dp_state Off");

    /* Depressurized MANUAL start runs until the dry-run guard trips. */
    CHECK(pm_request_on(&pm, 0, now), "manual on accepted");
    run(&pm, &now, 0.05f, 1, 200);
    CHECK(pm.state == PM_STATE_RUNNING && pm_relay_get(&pm) &&
          pm_dp_state_get(&pm) == 2, "manual: RUNNING, dp_state On");
    run(&pm, &now, 0.05f, 60, 200);      /* 12 s > dry_detect 10 s */
    CHECK(pm.state == PM_STATE_FAULT && pm.fault == PM_FAULT_DRY_RUN &&
          !pm_relay_get(&pm) && pm_dp_state_get(&pm) == 5,
          "dry run: fault latched, relay off, dp_state Fault");

    CHECK(!pm_request_on(&pm, 0, now), "fault latch refuses on-request");
    CHECK(!pm_fault_ack(&pm, false, now), "ack refused with a bad sensor");
    CHECK(pm_fault_ack(&pm, true, now) && pm.state == PM_STATE_IDLE,
          "ack with healthy sensor -> IDLE");
}

/* Safety: after a fault the operator can PRE-SELECT a safe mode BEFORE
 * acknowledging. Mode changes are accepted in FAULT (fault stays latched);
 * a subsequent ack then drops into IDLE in the chosen mode. In MANUAL the
 * pump must NOT restart even at low pressure (the AUTO-restart footgun). */
static void test_mode_change_in_fault(void)
{
    pm_config_t c = cfg_default();
    pm_t pm; uint64_t now = 1000;
    pm_init(&pm, &c);
    pm_update(&pm, 0.05f, true, now);

    /* Drive an AUTO dry-run fault at low pressure. */
    pm_mode_set(&pm, PM_MODE_AUTO, now);
    run(&pm, &now, 1.0f, 20, 200);       /* < on_bar: confirm -> RUNNING */
    CHECK(pm.state == PM_STATE_RUNNING, "auto: running before dry-run");
    run(&pm, &now, 1.0f, 60, 200);       /* no rise -> dry-run latch */
    CHECK(pm.state == PM_STATE_FAULT && pm.mode == PM_MODE_AUTO,
          "auto dry-run: latched, still AUTO");

    /* Pre-select MANUAL while latched: accepted, fault stays. */
    CHECK(pm_mode_set(&pm, PM_MODE_MANUAL, now),
          "mode change accepted in FAULT");
    CHECK(pm.state == PM_STATE_FAULT && pm.mode == PM_MODE_MANUAL,
          "still latched, mode now MANUAL");

    /* Ack -> IDLE in MANUAL: pump stays OFF despite low pressure. */
    CHECK(pm_fault_ack(&pm, true, now) && pm.state == PM_STATE_IDLE,
          "ack -> IDLE");
    run(&pm, &now, 1.0f, 10, 200);
    CHECK(pm.state == PM_STATE_IDLE && !pm_relay_get(&pm),
          "MANUAL after ack: no restart at low pressure");

    /* Also: pump_request_off equivalent (pm_request_off) works in FAULT. */
    pm_t pm2; uint64_t now2 = 1000;
    pm_init(&pm2, &c);
    pm_update(&pm2, 0.05f, true, now2);
    pm_mode_set(&pm2, PM_MODE_AUTO, now2);
    run(&pm2, &now2, 1.0f, 80, 200);
    CHECK(pm2.state == PM_STATE_FAULT, "second: dry-run latched");
    CHECK(pm_request_off(&pm2, now2) && pm2.state == PM_STATE_FAULT &&
          pm2.mode == PM_MODE_MANUAL,
          "off in FAULT: mode MANUAL, stays latched (ack not bypassed)");
}

static void test_no_start_above_off(void)
{
    pm_config_t c = cfg_default();
    pm_t pm; uint64_t now = 1000;
    pm_init(&pm, &c);
    run(&pm, &now, 4.0f, 2, 200);
    CHECK(!pm_request_on(&pm, 0, now), "no start above the OFF threshold");
}

static void test_auto_cycle_and_dip(void)
{
    pm_config_t c = cfg_default();
    pm_t pm; uint64_t now = 1000;
    pm_init(&pm, &c);
    run(&pm, &now, 3.0f, 2, 200);
    pm_mode_set(&pm, PM_MODE_AUTO, now);
    CHECK(pm_dp_state_get(&pm) == 3, "AUTO idle -> dp_state Auto");

    /* Brief dip below ON that recovers within the confirm window. */
    run(&pm, &now, 1.8f, 5, 200);        /* 1 s < confirm 3 s */
    CHECK(pm.state == PM_STATE_PRESSURE_DROP, "dip enters PRESSURE_DROP");
    run(&pm, &now, 3.0f, 2, 200);
    CHECK(pm.state == PM_STATE_IDLE && pm.m.cycles_total == 0,
          "brief dip: back to IDLE without a pump start");

    /* Confirmed drop -> start -> pump raises pressure -> full cycle. */
    run(&pm, &now, 1.8f, 20, 200);       /* 4 s > confirm */
    CHECK(pm.state == PM_STATE_RUNNING && pm.m.cycles_total == 1,
          "confirmed drop: pump started");
    float bar = 1.8f;
    for (int i = 0; i < 200 && pm.state == PM_STATE_RUNNING; i++) {
        bar += 0.02f;                     /* pump builds pressure */
        now += 200; pm_update(&pm, bar > 3.6f ? 3.6f : bar, true, now);
    }
    CHECK(pm.state == PM_STATE_RECOVERING && !pm_relay_get(&pm),
          "off threshold + min-on -> RECOVERING");
    run(&pm, &now, 3.6f, 10, 200);       /* stable */
    CHECK(pm.state == PM_STATE_LOCKOUT, "stable recovery -> LOCKOUT");
    run(&pm, &now, 3.6f, 25, 200);       /* 5 s > min_off 4 s */
    CHECK(pm.state == PM_STATE_IDLE, "lockout elapsed -> IDLE");
    CHECK(pm.m.event_duration_ms == 0 && pm.m.demand == PM_DEMAND_NONE,
          "event closed after the cycle");
}

static void test_demand_classification(void)
{
    pm_config_t c = cfg_default();
    pm_t pm; uint64_t now = 1000;
    pm_init(&pm, &c);
    run(&pm, &now, 3.0f, 2, 200);
    pm_mode_set(&pm, PM_MODE_AUTO, now);

    /* Plateau inside the hand band during the confirm phase. */
    run(&pm, &now, 1.85f, 25, 200);      /* 5 s at 1.85 bar (hand band) */
    CHECK(pm.state == PM_STATE_RUNNING && pm.m.demand == PM_DEMAND_HAND,
          "plateau 1.85 bar -> DEMAND_HAND");

    /* Unknown demand for too long -> leak suspicion + score. */
    pm_config_t c2 = cfg_default();
    c2.dry_min_rise_bar = 0.02f;         /* flat rise stays dry-run safe    */
    pm_t pm2; uint64_t now2 = 1000;
    pm_init(&pm2, &c2);
    run(&pm2, &now2, 3.0f, 2, 200);
    pm_mode_set(&pm2, PM_MODE_AUTO, now2);
    run(&pm2, &now2, 0.9f, 25, 200);     /* below both demand bands         */
    CHECK(pm2.state == PM_STATE_RUNNING, "unknown demand still starts");
    /* Hold ~25 s with a very slight rise (stays below the tank band, so
     * the plateau keeps classifying as UNKNOWN), > unknown_max 20 s. */
    float bar = 0.9f;
    for (int i = 0; i < 125; i++) {
        bar += 0.001f; now2 += 200;
        pm_update(&pm2, bar, true, now2);
        if (pm2.state != PM_STATE_RUNNING) break;
    }
    CHECK(pm2.state == PM_STATE_RUNNING &&
          pm2.m.demand == PM_DEMAND_LEAK_SUSPECT && pm2.m.anomaly_score >= 30,
          "unknown demand too long -> LEAK_SUSPECT + score");
}

static void test_critical_and_max_runtime(void)
{
    /* Critical low while running (pipe break) — was pressurized before. */
    pm_config_t c = cfg_default();
    pm_t pm; uint64_t now = 1000;
    pm_init(&pm, &c);
    run(&pm, &now, 3.0f, 2, 200);
    pm_mode_set(&pm, PM_MODE_AUTO, now);
    run(&pm, &now, 1.5f, 20, 200);       /* confirmed drop -> RUNNING */
    CHECK(pm.state == PM_STATE_RUNNING, "running before the break");
    run(&pm, &now, 0.1f, 3, 200);        /* collapses below critical */
    CHECK(pm.state == PM_STATE_FAULT && pm.fault == PM_FAULT_CRITICAL_LOW &&
          pm.m.demand == PM_DEMAND_PIPE_BREAK,
          "critical low while running -> PIPE_BREAK fault");

    /* Max runtime with held mid-band pressure. */
    pm_config_t c2 = cfg_default();
    c2.max_on_ms = 8000; c2.dry_detect_ms = 4000; c2.dry_min_rise_bar = 0.05f;
    pm_t pm2; uint64_t now2 = 1000;
    pm_init(&pm2, &c2);
    run(&pm2, &now2, 0.5f, 2, 200);
    CHECK(pm_request_on(&pm2, 0, now2), "manual on for max-runtime test");
    float bar = 0.5f;
    for (int i = 0; i < 60 && pm2.state == PM_STATE_RUNNING; i++) {
        bar += 0.01f;                    /* rising (dry-run safe), < off   */
        now2 += 200; pm_update(&pm2, bar, true, now2);
    }
    CHECK(pm2.state == PM_STATE_FAULT && pm2.fault == PM_FAULT_MAX_RUNTIME,
          "max runtime exceeded -> fault");
}

static void test_manual_off_respects_min_on(void)
{
    pm_config_t c = cfg_default();
    c.dry_detect_ms = 60000;             /* out of the way here */
    pm_t pm; uint64_t now = 1000;
    pm_init(&pm, &c);
    run(&pm, &now, 1.0f, 2, 200);
    CHECK(pm_request_on(&pm, 0, now), "manual on");
    run(&pm, &now, 1.0f, 5, 200);        /* 1 s elapsed */
    CHECK(pm_request_off(&pm, now), "off request accepted");
    run(&pm, &now, 1.0f, 5, 200);        /* 2 s: still under min_on 5 s */
    CHECK(pm_relay_get(&pm), "min-on holds the pump despite off request");
    run(&pm, &now, 1.0f, 25, 200);       /* past min_on */
    CHECK(!pm_relay_get(&pm), "pump stops once min-on is satisfied");
}

static void test_duration_and_starts_guard(void)
{
    pm_config_t c = cfg_default();
    c.dry_detect_ms = 60000; c.min_on_ms = 1000; c.min_off_ms = 400;
    c.max_starts_per_hour = 3;
    pm_t pm; uint64_t now = 1000;
    pm_init(&pm, &c);
    run(&pm, &now, 1.0f, 2, 200);

    /* turn_on_duration: stops when the deadline passes. */
    CHECK(pm_request_on(&pm, 2, now), "on for 2 s");
    run(&pm, &now, 1.0f, 6, 200);
    CHECK(pm_relay_get(&pm) && pm.m.remaining_s <= 2, "remaining time capped");
    run(&pm, &now, 1.0f, 6, 200);
    CHECK(!pm_relay_get(&pm), "duration elapsed -> pump off");

    /* Starts-per-hour guard: 3 allowed, the 4th is refused. */
    int slAccepted = 1;                  /* the duration start above */
    for (int i = 0; i < 3; i++) {
        run(&pm, &now, 1.0f, 30, 200);   /* recover + lockout + pause */
        if (pm_request_on(&pm, 1, now)) slAccepted++;
    }
    CHECK(slAccepted == 3, "hourly starts guard refuses the 4th start");
}

/* Timed-on is DECOUPLED from the Fon_Max_On_Time fault bound
 * (PM_MANUAL_MAX_ON_S): a duration > max_on_ms ends in a clean timed-off
 * at the deadline, NOT in a MAX_RUNTIME fault; the remaining time counts
 * toward the requested duration. Unlimited manual on keeps max_on_ms. */
static void test_timed_on_decoupled_from_max_runtime(void)
{
    pm_config_t c = cfg_default();
    c.dry_detect_ms = 600000;            /* dry-run guard out of the way (10 min) */
    c.min_on_ms = 1000; c.min_off_ms = 400;
    c.max_on_ms = 60000;                 /* 60 s — deliberately BELOW the duration */
    pm_t pm; uint64_t now = 1000;
    pm_init(&pm, &c);
    run(&pm, &now, 1.0f, 2, 200);

    /* (a)+(b) a 90 s timed-on survives the 60 s max_on bound and counts
     * down toward the REQUESTED duration. */
    CHECK(pm_request_on(&pm, 90, now), "on for 90 s accepted");
    run(&pm, &now, 1.0f, 5, 200);
    CHECK(pm.m.remaining_s > 60 && pm.m.remaining_s <= 90,
          "remaining counts toward requested 90 s (not capped at max_on)");
    run(&pm, &now, 1.0f, 330, 200);      /* -> t ~ 67 s: > max_on_ms        */
    CHECK(pm_relay_get(&pm) && pm.state != PM_STATE_FAULT,
          "still running past max_on_ms without MAX_RUNTIME fault");
    run(&pm, &now, 1.0f, 130, 200);      /* -> t ~ 93 s: deadline reached   */
    CHECK(!pm_relay_get(&pm) && pm.state != PM_STATE_FAULT,
          "clean timed-off at deadline, no fault latched");

    /* (c) Clamp: a request above the manual ceiling is capped. */
    run(&pm, &now, 1.0f, 60, 200);       /* recovery + min_off pause        */
    CHECK(pm_request_on(&pm, PM_MANUAL_MAX_ON_S + 100, now),
          "over-ceiling request accepted");
    run(&pm, &now, 1.0f, 2, 200);
    CHECK(pm.m.remaining_s <= PM_MANUAL_MAX_ON_S,
          "duration clamped to PM_MANUAL_MAX_ON_S");
    CHECK(pm_request_off(&pm, now), "off request accepted");
    run(&pm, &now, 1.0f, 40, 200);       /* wait out min_on -> off          */

    /* (d) Unlimited manual on (duration 0) STILL latches at
     * max_on_ms — the safety bound has not been weakened. */
    run(&pm, &now, 1.0f, 30, 200);       /* pause for min_off               */
    CHECK(pm_request_on(&pm, 0, now), "unlimited manual on accepted");
    run(&pm, &now, 1.0f, 320, 200);      /* > 60 s runtime                  */
    CHECK(pm.state == PM_STATE_FAULT && pm.fault == PM_FAULT_MAX_RUNTIME,
          "unlimited on still latches MAX_RUNTIME at max_on_ms");
}

int main(void)
{
    test_boot_and_manual_dry_run();
    test_mode_change_in_fault();
    test_no_start_above_off();
    test_auto_cycle_and_dip();
    test_demand_classification();
    test_critical_and_max_runtime();
    test_manual_off_respects_min_on();
    test_duration_and_starts_guard();
    test_timed_on_decoupled_from_max_runtime();

    if (s_slFails) { printf("%d TEST(S) FAILED\n", s_slFails); return 1; }
    printf("ALL TESTS PASSED (0 errors)\n");
    return 0;
}
