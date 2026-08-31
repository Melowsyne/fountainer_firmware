/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>

/* =============================================================
 * power_mgmt — two-state power model to lower the chip temperature
 * (safe operation at high ambient temperatures).
 *
 *  HIGH (default): CPU 160 MHz (PM lock held), WIFI_PS_NONE,
 *                  normal protocol intervals (heartbeat 30 s / report 10 s).
 *  LOW  (idle):    after POWER_IDLE_TO_LOW_S without an active server
 *                  request AND without an active task (pump idle, session
 *                  up, no OTA): PM lock released -> DFS scales the CPU
 *                  down to 80 MHz, modem sleep (WIFI_PS_MIN_MODEM), and
 *                  heartbeat/report stretched to a synchronized 60 s
 *                  (one modem wake-up per minute).
 *
 * Back to HIGH immediately on: command / dp_write / named dp_read,
 * OTA start, pump leaving idle, session loss (reconnect handshakes
 * should run at full clock). Mode is reported via System_Power_Mode.
 * ============================================================= */

#define POWER_IDLE_TO_LOW_S  300   /* 5 min without activity -> LOW */

/* Dependency inversion: power_mgmt lives in CORE and must not know the
 * network/device layers. MAIN injects these probes/actuators at startup:
 *   pump_idle    true when the pump rests (no active task)
 *   session_up   true while the server session is negotiated
 *   radio_ps_low enable/disable modem sleep
 *   proto_slow   stretch/restore the protocol intervals */
typedef struct {
    bool (*pump_idle)(void);
    bool (*session_up)(void);
    void (*radio_ps_low)(bool bLow);
    void (*proto_slow)(bool bSlow);
} power_mgmt_providers_t;

void power_mgmt_providers_set(const power_mgmt_providers_t *pstProviders);

void power_mgmt_init(void);

/* An active request/task arrived: refresh the 5-min window and leave LOW
 * immediately if necessary. Thread-safe (called from WS/OTA task context). */
void power_mgmt_activity_note(void);

/* Periodic evaluation (called from the main monitor task, ~5 s cycle). */
void power_mgmt_tick(void);

bool power_mgmt_low_get(void);
