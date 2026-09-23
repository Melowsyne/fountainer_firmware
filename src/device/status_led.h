/*
 * Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 *
 * This source code is proprietary. No license is granted to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of
 * this software without prior written permission from the copyright holder.
 *
 * For licensing inquiries, contact: info@melowsyne.com
 */

#pragma once
#include <stdbool.h>

/* =============================================================
 * status_led — the two configuration LEDs of the 24V board
 * (D6 red = IO15, D5 green = IO16, both ACTIVE LOW: anode via
 * series resistor to 3V3, cathode on the GPIO).
 *
 * Behavior (bring-up requirement Rev. 1):
 *   - Power-on: red + green light up for 5 s (esp_timer one-shot),
 *     then off.
 *   - SSR output switched on: green blinks at 1-s cadence.
 *   - Otherwise: green lights CONTINUOUSLY as long as the server
 *     communication is active (session established); off without
 *     a session.
 *
 * On boards without LED pins (hal_pins()->eLed* == GPIO_NUM_NC, i.e.
 * devkit HW1.x) all functions are side-effect-free no-ops.
 * ============================================================= */

/* Called from hal_setup(). true also on boards without LEDs. */
bool status_led_init(void);

/* Set the communication status (main glue from EVT_SESSION_READY/
 * EVT_SESSION_LOST/EVT_WLAN_DISCONNECTED). During the 5-s boot
 * display the state is only remembered and applied afterwards.
 * Thread-safe (event-dispatch vs. esp_timer task). */
void status_led_comm_active(bool bActive);

/* Set the SSR/relay state (from hal_relay_set; idempotent, called in
 * every pump cycle). Relay on -> green blinks, takes priority over
 * the steady light of the session display. */
void status_led_relay_active(bool bOn);
