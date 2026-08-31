/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* =============================================================
 * wlan_com module — basic WLAN connection setup.
 *
 * Note on how it works: WLAN does NOT run via its own
 * interrupt and needs no own polling task.
 * ESP-IDF runs the WLAN driver and lwIP in internal FreeRTOS
 * tasks and reports states via the event loop system
 * (WIFI_EVENT / IP_EVENT). This module only registers the
 * event handlers and initiates connect/reconnect.
 * ============================================================= */

typedef enum {
    WLAN_DISCONNECTED = 0,
    WLAN_CONNECTING,
    WLAN_CONNECTED,        /* IP obtained */
} wlan_state_t;

/* Observer-style callback for state changes (e.g. for task_com). */
typedef void (*wlan_state_cb_t)(wlan_state_t eState, void *pvCtx);

bool wlan_com_init(void);
bool wlan_com_connect(const char *pstrSsid, const char *pstrPassword);
bool wlan_com_connected_get(void);

/* Unexpected link losses since boot (each schedules a reconnect); the clean
 * pre-reboot teardown is NOT counted. Feeds the System_Reconnect_Count DP. */
uint32_t wlan_com_disconnect_count_get(void);

/* Power saving: true -> modem sleep (WIFI_PS_MIN_MODEM, radio naps between
 * DTIM beacons); false -> WIFI_PS_NONE (full responsiveness; also used
 * during association, where power save can drop auth/assoc frames). */
void wlan_com_ps_low_set(bool bLow);
bool wlan_com_saved_credentials_get(char *pstrSsid, size_t szSsid,
                                    char *pstrPassword, size_t szPassword);
void wlan_com_state_cb_set(wlan_state_cb_t pfnCb, void *pvCtx);

/* Clean WLAN teardown (protected deauth) before a software reboot. Waits
 * up to ulTimeoutMs for the disconnect confirmation, so that the AP releases the
 * 802.11w SA immediately -> fast reconnect after the restart (instead of ~3 min). */
void wlan_com_disconnect_blocking(uint32_t ulTimeoutMs);

/* Complete WLAN shutdown before esp_restart(): disconnect_blocking() plus
 * esp_wifi_stop(). SINGLE place for the safety-relevant reboot sequence, so
 * that every reboot path (command "reboot", OTA apply, ...) tears down
 * identically and the fast post-reboot reconnect is preserved. */
void wlan_com_teardown_for_reboot(uint32_t ulTimeoutMs);
