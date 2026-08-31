/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================
 * app_watchdog — channel-based PROGRESS supervision with staged
 * recovery (Watchdog_v1.md, work package 4).
 *
 * Principles:
 *   - a heartbeat is reported only after REAL, completed work
 *     (never fed blindly at loop tops)
 *   - per channel: deadline -> timeout -> soft recovery (injected
 *     action, e.g. WS-client restart) -> after max_soft failures
 *     escalate to a device reboot (optional, gated by a probe
 *     such as "physical link up")
 *   - reboot-loop BRAKE: an RTC counter of watchdog reboots
 *     without an intervening session success; at 3 the watchdog
 *     stops rebooting and keeps soft-recovering until power cycle
 *   - reboot reason (channel/checkpoint) survives in RTC_NOINIT
 *     and is published as a boot diagnosis after restart
 *
 * This component is the ONLY recovery/reboot decision maker; the
 * ESP-IDF TWDT (60 s, PANIC) is the hard fallback BELOW it for
 * busy-hangs the app watchdog itself cannot see. Reboots go
 * through an injected hook (core/system in this project) — the
 * component knows no upper layer. Channel IDs/definitions are
 * project knowledge (src/main/watchdog_table.c).
 * ============================================================= */

#define WD_MAX_CHANNELS        8
#define WD_CHECK_PERIOD_MS  2000
#define WD_REBOOT_BRAKE_MAX    3   /* WD reboots without a session success */

typedef struct {
    const char *name;
    uint32_t deadline_ms;          /* timeout without a heartbeat            */
    uint8_t  max_soft;             /* soft recoveries before escalation      */
    bool (*recover)(void *arg);    /* stage-1 action; NULL = warn-only stage */
    bool (*reboot_allowed)(void *arg); /* escalation gate; NULL = always     */
    bool escalate_reboot;          /* may escalate to a device reboot        */
    void *arg;
} wd_channel_def_t;

typedef struct {
    bool     bValid;               /* last reboot was a watchdog reboot      */
    uint8_t  ucChannel;
    uint16_t usCheckpoint;
    uint8_t  ucSoftCount;
    uint32_t ulUptimeS;            /* uptime at the moment of the reboot     */
    uint8_t  ucRebootsWithoutSuccess;  /* brake counter (incl. this boot)    */
} wd_boot_diag_t;

/* Reads/initializes the RTC diagnosis — call FIRST (before any register). */
esp_err_t wd_init(void);

/* Reboot hook injection (main wires core/system's deferred reboot). */
void wd_reboot_hook_set(bool (*pfnReboot)(uint32_t ulDelayMs));

esp_err_t wd_register(uint32_t ulId, const wd_channel_def_t *pstDef);

/* Starts the 2-s check task (subscribes itself to the TWDT). */
esp_err_t wd_start(void);

/* Progress notification — ONLY after completed work. Resets the channel's
 * deadline and (after a recovery) reports RECOVERY_SUCCESS. */
void wd_heartbeat(uint32_t ulId);

/* Last position marker per channel (persisted with the reboot reason). */
void wd_checkpoint(uint32_t ulId, uint16_t usCheckpoint);

/* Application-level success (session established): clears the reboot brake. */
void wd_session_success_note(void);

/* Boot diagnosis of the previous run; false if the last reboot was not
 * watchdog-initiated (power-on, OTA, commanded reboot, panic). */
bool wd_boot_diag_get(wd_boot_diag_t *pstOut);

#ifdef __cplusplus
}
#endif
