/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

/* =============================================================
 * core/system — the ONE deferred device-reboot path.
 *
 * Callers (command "reboot", Network_Save=2, watchdogs) request the
 * reboot here; a hook injected by the MAIN module (dependency
 * inversion — core must not know the network layer) performs the
 * clean pre-reboot work (WLAN deauth/teardown) before esp_restart().
 * ============================================================= */

/* Injected by main at startup (e.g. wlan teardown). May be NULL. */
void system_pre_reboot_hook_set(void (*pfnHook)(void));

/* Reboot in its own task after ulDelayMs (lets replies/logs drain first).
 * Returns false if the reboot task could not be created. */
bool system_reboot_deferred(uint32_t ulDelayMs);
