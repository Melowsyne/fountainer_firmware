/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* Bridges into the logging core:
 *  1. event bridge — subscribes SELECTED system events and turns them into
 *     structured records (not every event is logged; high-frequency data
 *     stays out).
 *  2. esp_log hook — captures ESP-IDF's own W/E lines (esp-tls, wifi,
 *     websocket_client...) via esp_log_set_vprintf; experience shows these
 *     are the most valuable diagnostics of this project. The original
 *     vprintf keeps running (console output unchanged).
 * Runs in the event-manager dispatch task / the logging caller's task —
 * emit is short (level gate + ring copy). */

#include "logging.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "event_manager.h"

/* ---- 1. Event bridge ---------------------------------------------------- */

typedef struct {
    system_event_t eEvent;
    log_level_t    eLevel;
    uint8_t        ucModule;
    uint16_t       usLogEvent;
    const char    *pstrText;
} bridge_map_t;

static const bridge_map_t s_astMap[] = {
    { EVT_SYSTEM_BOOT,           LOG_LEVEL_INFO,  LOG_MOD_SYSTEM, LOG_EVT_BOOT,                 "boot" },
    { EVT_SYSTEM_READY,          LOG_LEVEL_INFO,  LOG_MOD_SYSTEM, LOG_EVT_SYSTEM_READY,         "system ready" },
    { EVT_LOW_MEMORY,            LOG_LEVEL_WARN,  LOG_MOD_SYSTEM, LOG_EVT_LOW_MEMORY,           "low memory" },
    { EVT_FATAL_ERROR,           LOG_LEVEL_ERROR, LOG_MOD_SYSTEM, LOG_EVT_FATAL,                "fatal" },
    { EVT_WLAN_CONNECTED,        LOG_LEVEL_INFO,  LOG_MOD_WLAN,   LOG_EVT_WLAN_CONNECTED,       "wlan connected" },
    { EVT_WLAN_DISCONNECTED,     LOG_LEVEL_WARN,  LOG_MOD_WLAN,   LOG_EVT_WLAN_DISCONNECTED,    "wlan disconnected" },
    { EVT_SESSION_READY,         LOG_LEVEL_INFO,  LOG_MOD_WLAN,   LOG_EVT_SESSION_READY,        "session ready" },
    { EVT_SESSION_LOST,          LOG_LEVEL_WARN,  LOG_MOD_WLAN,   LOG_EVT_SESSION_LOST,         "session lost" },
    { EVT_OTA_STARTED,           LOG_LEVEL_INFO,  LOG_MOD_OTA,    LOG_EVT_OTA_STARTED,          NULL },
    { EVT_OTA_APPLIED,           LOG_LEVEL_INFO,  LOG_MOD_OTA,    LOG_EVT_OTA_APPLIED,          NULL },
    { EVT_OTA_FAILED,            LOG_LEVEL_ERROR, LOG_MOD_OTA,    LOG_EVT_OTA_FAILED,           "ota failed" },
    { EVT_OTA_REJECTED_UNSIGNED, LOG_LEVEL_ERROR, LOG_MOD_OTA,    LOG_EVT_OTA_REJECTED_UNSIGNED,"ota unsigned rejected" },
    { EVT_PUMP_STATE_CHANGED,    LOG_LEVEL_INFO,  LOG_MOD_PUMP,   LOG_EVT_PUMP_STATE,           "pump state" },
    { EVT_PUMP_FAULT,            LOG_LEVEL_ERROR, LOG_MOD_PUMP,   LOG_EVT_PUMP_FAULT,           "pump fault" },
    { EVT_PUMP_FAULT_CLEARED,    LOG_LEVEL_INFO,  LOG_MOD_PUMP,   LOG_EVT_PUMP_FAULT_CLEARED,   "pump fault cleared" },
    { EVT_NETWORK_CONFIG_SAVED,  LOG_LEVEL_INFO,  LOG_MOD_SYSTEM, LOG_EVT_CONFIG_SAVED,         "config saved" },
    { EVT_NETWORK_CONFIG_RESTORED,LOG_LEVEL_WARN, LOG_MOD_SYSTEM, LOG_EVT_CONFIG_RESTORED,      "config restored" },
    { EVT_POWER_MODE_CHANGED,    LOG_LEVEL_INFO,  LOG_MOD_SYSTEM, LOG_EVT_POWER_MODE,           "power mode" },
    { EVT_DP_WRITTEN,            LOG_LEVEL_DEBUG, LOG_MOD_SYSTEM, LOG_EVT_DP_WRITTEN,           "dp write" },
    { EVT_TASK_STARTED,          LOG_LEVEL_DEBUG, LOG_MOD_TASK,   LOG_EVT_TASK_STARTED,         "task started" },
    { EVT_TASK_STOPPED,          LOG_LEVEL_INFO,  LOG_MOD_TASK,   LOG_EVT_TASK_STOPPED,         "task stopped" },
    { EVT_TASK_CYCLE_OVERRUN,    LOG_LEVEL_WARN,  LOG_MOD_TASK,   LOG_EVT_TASK_CYCLE_OVERRUN,   "cycle overrun" },
    { EVT_TASK_STACK_LOW,        LOG_LEVEL_WARN,  LOG_MOD_TASK,   LOG_EVT_TASK_STACK_LOW,       "stack low" },
};

static const bridge_map_t *map_find(system_event_t eEvent)
{
    for (size_t i = 0; i < sizeof(s_astMap) / sizeof(s_astMap[0]); i++)
        if (s_astMap[i].eEvent == eEvent) return &s_astMap[i];
    return NULL;
}

static void bridge_cb(system_event_t eEvent, const void *pvData, size_t szSize)
{
    const bridge_map_t *pstMap = map_find(eEvent);
    if (!pstMap || !logging_is_enabled_fast(pstMap->eLevel)) return;

    int32_t aslArgs[LOG_MAX_ARGS] = {0};
    uint8_t ucArgc = 0;
    const char *pstrText = pstMap->pstrText;
    char strBuf[LOG_MAX_TEXT];

    /* Payload decoding per event family (see system_events.h). */
    switch (eEvent) {
        case EVT_SYSTEM_BOOT:
        case EVT_WLAN_DISCONNECTED:
        case EVT_PUMP_FAULT:
        case EVT_NETWORK_CONFIG_SAVED:
        case EVT_POWER_MODE_CHANGED:
        case EVT_DP_WRITTEN:
            if (pvData && szSize >= 1) { aslArgs[0] = *(const uint8_t *)pvData; ucArgc = 1; }
            break;
        case EVT_WLAN_CONNECTED:
        case EVT_LOW_MEMORY:                     /* u32 payload (ip / bytes) */
            if (pvData && szSize >= 4) {
                uint32_t ulV; memcpy(&ulV, pvData, 4);
                aslArgs[0] = (int32_t)ulV; ucArgc = 1;
            }
            break;
        case EVT_OTA_STARTED:
        case EVT_OTA_APPLIED:
            if (pvData && szSize) {          /* version string as text */
                size_t n = szSize < sizeof(strBuf) - 1 ? szSize : sizeof(strBuf) - 1;
                memcpy(strBuf, pvData, n); strBuf[n] = '\0';
                pstrText = strBuf;
            }
            break;
        case EVT_OTA_FAILED:
            if (pvData && szSize >= sizeof(evt_ota_failed_t)) {
                const evt_ota_failed_t *p = pvData;
                aslArgs[0] = p->ucPhase; aslArgs[1] = p->slEspErr; ucArgc = 2;
            }
            break;
        case EVT_PUMP_STATE_CHANGED:
            if (pvData && szSize >= sizeof(evt_pump_state_t)) {
                const evt_pump_state_t *p = pvData;
                aslArgs[0] = p->ucOld; aslArgs[1] = p->ucNew; ucArgc = 2;
            }
            break;
        case EVT_TASK_STARTED:
        case EVT_TASK_STOPPED:
        case EVT_TASK_CYCLE_OVERRUN:
        case EVT_TASK_STACK_LOW:
            if (pvData && szSize >= sizeof(evt_task_info_t)) {
                const evt_task_info_t *p = pvData;
                aslArgs[0] = p->ucTaskId; aslArgs[1] = p->slReason; ucArgc = 2;
            }
            break;
        default:
            break;
    }

    logging_emit(pstMap->eLevel, pstMap->ucModule, pstMap->usLogEvent,
                 aslArgs, ucArgc, pstrText);
}

void logging_bridge_subscribe_events(void)
{
    for (size_t i = 0; i < sizeof(s_astMap) / sizeof(s_astMap[0]); i++)
        event_manager_subscribe(s_astMap[i].eEvent, bridge_cb);
}

/* ---- 2. esp_log capture -------------------------------------------------- */

static vprintf_like_t s_pfnPrevVprintf;

static int esp_log_capture(const char *pstrFmt, va_list xArgs)
{
    /* Always keep the console output (chained original vprintf). */
    va_list xCopy;
    va_copy(xCopy, xArgs);
    int slRet = s_pfnPrevVprintf ? s_pfnPrevVprintf(pstrFmt, xCopy)
                                 : vprintf(pstrFmt, xCopy);
    va_end(xCopy);

    /* IDF lines start with "E (ms) tag: ..." / "W (ms) tag: ...". Only
     * W/E are captured (INFO noise stays out of the ring). */
    char cLead = pstrFmt[0];
    log_level_t eLevel;
    if      (cLead == 'E') eLevel = LOG_LEVEL_ERROR;
    else if (cLead == 'W') eLevel = LOG_LEVEL_WARN;
    else return slRet;
    if (!logging_is_enabled_fast(eLevel)) return slRet;

    char strLine[LOG_MAX_TEXT];
    vsnprintf(strLine, sizeof(strLine), pstrFmt, xArgs);
    /* Strip a trailing newline; drop the "E (12345) " lead-in if present. */
    char *pstrMsg = strchr(strLine, ')');
    pstrMsg = (pstrMsg && pstrMsg[1] == ' ') ? pstrMsg + 2 : strLine;
    size_t szLen = strlen(pstrMsg);
    while (szLen && (pstrMsg[szLen - 1] == '\n' || pstrMsg[szLen - 1] == '\r'))
        pstrMsg[--szLen] = '\0';
    if (szLen == 0) return slRet;

    logging_emit(eLevel, LOG_MOD_IDF, LOG_EVT_IDF_LINE, NULL, 0, pstrMsg);
    return slRet;
}

void logging_bridge_hook_esp_log(void)
{
    s_pfnPrevVprintf = esp_log_set_vprintf(esp_log_capture);
}
