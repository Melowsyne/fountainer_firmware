/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================
 * logging — structured, switchable diagnostics records
 * (Logging_v1.md, work package 3).
 *
 *   RAM ring (static, 32 KiB): runtime records, pulled by the
 *   Linux server via the protocol messages log_read/log_batch.
 *   Flash tier (previous-boot log): DISABLED until the logstore
 *   partition exists (USB milestone, package 5) — the API is
 *   already in place and reports "not available".
 *
 * Records carry semantics in (module_id, event_id, args[]); the
 * short text is a diagnosis aid, the server renders the human-
 * readable message. The emit path is short and deterministic:
 * fast level gate -> fill record -> ring under a short critical
 * section. No heap, no flash, no network in the log path — and
 * NO power_mgmt activity (logging must not block the LOW mode).
 * ============================================================= */

typedef enum {
    LOG_LEVEL_OFF   = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_INFO  = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_TRACE = 5,
} log_level_t;

typedef enum {
    LOG_MOD_SYSTEM = 1,
    LOG_MOD_WLAN   = 2,
    LOG_MOD_PUMP   = 3,
    LOG_MOD_OTA    = 4,
    LOG_MOD_EVENT  = 5,
    LOG_MOD_TASK   = 6,
    LOG_MOD_APP    = 7,   /* legacy logging() facade (core/debug.c)      */
    LOG_MOD_IDF    = 8,   /* captured ESP-IDF logs (esp-tls, wifi, ...)  */
} log_module_id_t;

/* Event-IDs are stable (the server interprets them). */
typedef enum {
    LOG_EVT_TEXT = 1,                 /* free text via the logging() facade */

    LOG_EVT_BOOT = 100,               /* args: reset_reason                 */
    LOG_EVT_SYSTEM_READY,
    LOG_EVT_LOW_MEMORY,               /* args: free heap bytes              */
    LOG_EVT_FATAL,
    LOG_EVT_CLIMATE_FAIL = 120,       /* args: consecutive misses, 1=recov. */
    LOG_EVT_NVS_SAVE_FAIL,            /* config applied in RAM only         */
    LOG_EVT_DHT_SCAN,                 /* wiring debug: args probed pin,
                                         ok<<8|phase (see onewire_am2302)   */
    LOG_EVT_DHT_RAW,                  /* RETIRED (was: raw-frame sensor
                                         debug) — id stays reserved         */

    LOG_EVT_WLAN_CONNECTED = 200,     /* args: ip4                          */
    LOG_EVT_WLAN_DISCONNECTED,        /* args: reason                       */
    LOG_EVT_SESSION_READY,
    LOG_EVT_SESSION_LOST,
    LOG_EVT_LINK_POOR = 210,          /* args: score, rssi                  */
    LOG_EVT_LINK_RECOVERED,           /* args: score, rssi                  */
    LOG_EVT_NET_SAMPLE,               /* time series: rssi, score,
                                         tx_fail_total, session_drops       */

    LOG_EVT_OTA_STARTED = 300,        /* text: version                      */
    LOG_EVT_OTA_APPLIED,
    LOG_EVT_OTA_FAILED,               /* args: phase, esp_err               */
    LOG_EVT_OTA_REJECTED_UNSIGNED,

    LOG_EVT_PUMP_STATE = 400,         /* args: old, new                     */
    LOG_EVT_PUMP_FAULT,               /* args: fault_code                   */
    LOG_EVT_PUMP_FAULT_CLEARED,
    LOG_EVT_PM_SAMPLE = 410,          /* time series (DEBUG): args mbar_filt,
                                         slope_mbar_s, state<<8|demand, score */
    LOG_EVT_PM_EVENT,                 /* event summary: args dur_s, min_mbar,
                                         demand, volume_dl                   */
    LOG_EVT_PM_LABEL,                 /* args: manual event label            */
    LOG_EVT_PM_RELAY_FAIL,            /* args: wanted state, 1=recovered     */

    LOG_EVT_CONFIG_SAVED = 500,       /* args: action                       */
    LOG_EVT_CONFIG_RESTORED,
    LOG_EVT_POWER_MODE,               /* args: mode                         */
    LOG_EVT_DP_WRITTEN,               /* args: count                        */

    LOG_EVT_TASK_STARTED = 600,       /* args: task_id                      */
    LOG_EVT_TASK_STOPPED,
    LOG_EVT_TASK_CYCLE_OVERRUN,       /* args: task_id, runtime_ms          */
    LOG_EVT_TASK_STACK_LOW,           /* args: task_id, words_free          */

    LOG_EVT_WD_TIMEOUT = 700,         /* args: channel, soft_count          */
    LOG_EVT_WD_RECOVERED,             /* args: channel, soft_count          */
    LOG_EVT_WD_REBOOT,                /* args: channel, checkpoint          */
    LOG_EVT_WD_BRAKE,                 /* args: channel, reboots             */
    LOG_EVT_WD_BOOT_DIAG,             /* args: channel, checkpoint          */

    LOG_EVT_IDF_LINE = 800,           /* captured ESP-IDF W/E line (text)   */
} log_event_id_t;

#define LOG_MAX_ARGS      4
#define LOG_MAX_TEXT      48
#define LOG_RING_BYTES    (32 * 1024)

typedef struct {
    uint32_t ulSeq;
    uint32_t ulBootId;
    uint32_t ulUptimeMs;
    uint16_t usEventId;
    uint8_t  ucModuleId;
    uint8_t  ucLevel;
    uint8_t  ucArgc;
    int32_t  aslArgs[LOG_MAX_ARGS];
    char     strText[LOG_MAX_TEXT];
} log_record_t;

typedef struct {
    uint32_t ulBootId;
    uint32_t ulFirstSeqAvailable;   /* oldest seq still in the ring        */
    uint32_t ulNextSeq;
    uint32_t ulDropped;             /* overwritten records since boot      */
} log_stats_t;

/* Early init (before dp/network): boot_id, ring, defaults. */
void logging_init_early(void);

/* Fast gate — call before building args (cheap when disabled). */
bool logging_is_enabled_fast(log_level_t eLevel);

void logging_emit(log_level_t eLevel, uint8_t ucModuleId, uint16_t usEventId,
                  const int32_t *pslArgs, uint8_t ucArgc, const char *pstrText);

/* Copy records with seq > ulSinceSeq and level <= eMinLevel-gate into pstOut.
 * Returns the number copied; stats (incl. overflow detection data) via out. */
size_t logging_read_since(uint32_t ulSinceSeq, log_level_t eMinLevel,
                          log_record_t *pstOut, size_t szMax,
                          log_stats_t *pstStats);

void logging_stats_get(log_stats_t *pstStats);
void logging_clear_runtime(void);

/* Runtime configuration (fed from the Log_* datapoints by the app). */
void logging_set_enabled(bool bEnabled);
void logging_set_runtime_level(log_level_t eLevel);
void logging_set_flash_level(log_level_t eLevel);

/* Flash tier (logstore partition; RUNTIME-DETECTED — without the partition
 * every call is a no-op and previous_boot_available stays false). Persists
 * records at/below Log_Flash_Level plus boot/wd-diagnosis records; the
 * previous boot's slot is served until the server acknowledges it. */
void   logging_flash_init(void);            /* after logging_init_early     */
void   logging_flash_submit(const log_record_t *pstRec);   /* internal      */
uint32_t logging_flash_dropped_get(void);
bool   logging_previous_boot_available(uint32_t *pulBootId);
size_t logging_read_previous_boot(log_level_t eMinLevel,
                                  log_record_t *pstOut, size_t szMax);
bool   logging_ack_previous_boot(uint32_t ulBootId);

/* Bridges (implemented in logging_bridge.c; called once at start). */
void logging_bridge_subscribe_events(void);
void logging_bridge_hook_esp_log(void);

/* Typed convenience macros (fast gate first). */
#define LOG_EMIT0(lvl, mod, ev, txt)                                     \
    do { if (logging_is_enabled_fast(lvl))                               \
             logging_emit((lvl), (mod), (ev), NULL, 0, (txt)); } while (0)
#define LOG_EMIT2(lvl, mod, ev, a0, a1, txt)                             \
    do { if (logging_is_enabled_fast(lvl)) {                             \
             int32_t _a[2] = { (int32_t)(a0), (int32_t)(a1) };           \
             logging_emit((lvl), (mod), (ev), _a, 2, (txt)); } } while (0)
#define LOG_EMIT4(lvl, mod, ev, a0, a1, a2, a3, txt)                     \
    do { if (logging_is_enabled_fast(lvl)) {                             \
             int32_t _a[4] = { (int32_t)(a0), (int32_t)(a1),             \
                               (int32_t)(a2), (int32_t)(a3) };           \
             logging_emit((lvl), (mod), (ev), _a, 4, (txt)); } } while (0)

#ifdef __cplusplus
}
#endif
