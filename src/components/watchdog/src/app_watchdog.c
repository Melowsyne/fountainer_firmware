/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "app_watchdog.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "event_manager.h"
#include "logging.h"

static const char *TAG = "app_wd";

/* Reboot reason across a software reset (NOT across power-on). */
#define WD_RTC_MAGIC 0x57444F47u   /* "WDOG" */
typedef struct {
    uint32_t ulMagic;
    uint8_t  ucPending;            /* 1 = next boot is a watchdog reboot     */
    uint8_t  ucChannel;
    uint16_t usCheckpoint;
    uint8_t  ucSoftCount;
    uint8_t  ucRebootsWithoutSuccess;
    uint32_t ulUptimeS;
} wd_rtc_t;
RTC_NOINIT_ATTR static wd_rtc_t s_stRtc;

typedef struct {
    bool bUsed;
    wd_channel_def_t stDef;
    int64_t  llLastBeatUs;
    uint16_t usCheckpoint;
    uint8_t  ucSoftCount;          /* soft recoveries since the last beat    */
    bool     bInTimeout;           /* deadline currently exceeded            */
    bool     bBrakeLogged;         /* brake message emitted once             */
} wd_channel_t;

static wd_channel_t  s_astCh[WD_MAX_CHANNELS];
static wd_boot_diag_t s_stDiag;
static bool (*s_pfnReboot)(uint32_t ulDelayMs);
static TaskHandle_t  s_hTask;

/* Heartbeats arrive from arbitrary tasks while the check task reads them —
 * the 64-bit timestamp is NOT atomic on Xtensa (torn read = false or
 * missed timeout). Short critical section around beat state only; actions
 * (log/publish/recover/reboot) always run OUTSIDE it. */
static portMUX_TYPE s_xBeatLock = portMUX_INITIALIZER_UNLOCKED;

static void publish_wd_event(system_event_t eEvent, uint32_t ulId, int slInfo)
{
    evt_task_info_t stInfo = { .ucTaskId = (unsigned char)ulId, .slReason = slInfo };
    event_manager_publish(eEvent, &stInfo, sizeof(stInfo));
}

esp_err_t wd_init(void)
{
    memset(s_astCh, 0, sizeof(s_astCh));
    memset(&s_stDiag, 0, sizeof(s_stDiag));

    if (s_stRtc.ulMagic != WD_RTC_MAGIC) {
        /* Power-on / brown-out: RTC content is garbage -> fresh start,
         * which also releases the reboot brake. */
        memset(&s_stRtc, 0, sizeof(s_stRtc));
        s_stRtc.ulMagic = WD_RTC_MAGIC;
        return ESP_OK;
    }
    if (s_stRtc.ucPending) {
        s_stDiag.bValid       = true;
        s_stDiag.ucChannel    = s_stRtc.ucChannel;
        s_stDiag.usCheckpoint = s_stRtc.usCheckpoint;
        s_stDiag.ucSoftCount  = s_stRtc.ucSoftCount;
        s_stDiag.ulUptimeS    = s_stRtc.ulUptimeS;
        s_stDiag.ucRebootsWithoutSuccess = s_stRtc.ucRebootsWithoutSuccess;
        s_stRtc.ucPending = 0;
    }
    return ESP_OK;
}

void wd_reboot_hook_set(bool (*pfnReboot)(uint32_t ulDelayMs))
{
    s_pfnReboot = pfnReboot;
}

esp_err_t wd_register(uint32_t ulId, const wd_channel_def_t *pstDef)
{
    if (ulId >= WD_MAX_CHANNELS || !pstDef || !pstDef->name ||
        pstDef->deadline_ms == 0)
        return ESP_ERR_INVALID_ARG;
    if (s_astCh[ulId].bUsed) return ESP_ERR_INVALID_STATE;

    s_astCh[ulId].bUsed = true;
    s_astCh[ulId].stDef = *pstDef;
    s_astCh[ulId].llLastBeatUs = esp_timer_get_time();
    return ESP_OK;
}

void wd_heartbeat(uint32_t ulId)
{
    if (ulId >= WD_MAX_CHANNELS || !s_astCh[ulId].bUsed) return;
    wd_channel_t *pstCh = &s_astCh[ulId];

    bool bWasInTimeout;
    uint8_t ucSoft;
    portENTER_CRITICAL(&s_xBeatLock);
    bWasInTimeout = pstCh->bInTimeout;
    ucSoft        = pstCh->ucSoftCount;
    pstCh->llLastBeatUs = esp_timer_get_time();
    pstCh->ucSoftCount  = 0;
    pstCh->bInTimeout   = false;
    pstCh->bBrakeLogged = false;
    portEXIT_CRITICAL(&s_xBeatLock);

    if (bWasInTimeout) {
        ESP_LOGI(TAG, "channel '%s' recovered", pstCh->stDef.name);
        publish_wd_event(EVT_WD_RECOVERY_SUCCESS, ulId, ucSoft);
        LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_SYSTEM, LOG_EVT_WD_RECOVERED,
                  ulId, ucSoft, pstCh->stDef.name);
    }
}

void wd_checkpoint(uint32_t ulId, uint16_t usCheckpoint)
{
    if (ulId >= WD_MAX_CHANNELS || !s_astCh[ulId].bUsed) return;
    s_astCh[ulId].usCheckpoint = usCheckpoint;
}

void wd_session_success_note(void)
{
    if (s_stRtc.ucRebootsWithoutSuccess)
        ESP_LOGI(TAG, "session success — reboot brake reset (was %u)",
                 (unsigned)s_stRtc.ucRebootsWithoutSuccess);
    s_stRtc.ucRebootsWithoutSuccess = 0;
}

bool wd_boot_diag_get(wd_boot_diag_t *pstOut)
{
    if (pstOut) *pstOut = s_stDiag;
    return s_stDiag.bValid;
}

/* Escalation: persist the reason in RTC, then reboot via the injected hook.
 * Gated by the channel's probe AND the reboot-loop brake. */
static void escalate(uint32_t ulId, wd_channel_t *pstCh)
{
    if (pstCh->stDef.reboot_allowed &&
        !pstCh->stDef.reboot_allowed(pstCh->stDef.arg)) {
        /* e.g. no physical link: a reboot cannot help; the lower layers
         * keep retrying — stay in soft recovery. */
        return;
    }
    if (s_stRtc.ucRebootsWithoutSuccess >= WD_REBOOT_BRAKE_MAX) {
        if (!pstCh->bBrakeLogged) {
            ESP_LOGE(TAG, "channel '%s': reboot brake active (%u WD reboots "
                     "without success) — no further reboots until power cycle",
                     pstCh->stDef.name, (unsigned)s_stRtc.ucRebootsWithoutSuccess);
            LOG_EMIT2(LOG_LEVEL_ERROR, LOG_MOD_SYSTEM, LOG_EVT_WD_BRAKE,
                      ulId, s_stRtc.ucRebootsWithoutSuccess, pstCh->stDef.name);
            publish_wd_event(EVT_WD_RECOVERY_FAILED, ulId,
                             s_stRtc.ucRebootsWithoutSuccess);
            pstCh->bBrakeLogged = true;
        }
        return;
    }
    if (!s_pfnReboot) {
        ESP_LOGE(TAG, "no reboot hook injected — cannot escalate");
        return;
    }

    s_stRtc.ucPending     = 1;
    s_stRtc.ucChannel     = (uint8_t)ulId;
    s_stRtc.usCheckpoint  = pstCh->usCheckpoint;
    s_stRtc.ucSoftCount   = pstCh->ucSoftCount;
    s_stRtc.ulUptimeS     = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_stRtc.ucRebootsWithoutSuccess++;

    ESP_LOGE(TAG, "channel '%s': %u soft recoveries exhausted — rebooting "
             "(WD reboot %u/%u)", pstCh->stDef.name, (unsigned)pstCh->ucSoftCount,
             (unsigned)s_stRtc.ucRebootsWithoutSuccess, WD_REBOOT_BRAKE_MAX);
    LOG_EMIT2(LOG_LEVEL_ERROR, LOG_MOD_SYSTEM, LOG_EVT_WD_REBOOT,
              ulId, pstCh->usCheckpoint, pstCh->stDef.name);
    publish_wd_event(EVT_WD_REBOOT_REQUESTED, ulId, pstCh->usCheckpoint);

    if (!s_pfnReboot(2000))            /* 2 s: let log_read/events drain */
        s_stRtc.ucPending = 0;         /* hook refused -> keep running   */
}

static void check_channel(uint32_t ulId, wd_channel_t *pstCh, int64_t llNowUs)
{
    /* Decide + re-arm under the beat lock; ACT outside of it (logging,
     * events, recover and reboot calls may block). */
    bool bSoftStage = false;
    bool bEscalate  = false;
    uint32_t ulElapsedMs;
    uint8_t  ucSoft;

    portENTER_CRITICAL(&s_xBeatLock);
    ulElapsedMs = (uint32_t)((llNowUs - pstCh->llLastBeatUs) / 1000);
    if (ulElapsedMs < pstCh->stDef.deadline_ms) {
        portEXIT_CRITICAL(&s_xBeatLock);
        return;
    }
    /* Deadline exceeded. Each period of silence runs ONE stage; every
     * branch re-arms the full deadline (matches the proven 180-s restart
     * grid of the former connectivity watchdog). */
    pstCh->bInTimeout = true;
    if (pstCh->ucSoftCount < pstCh->stDef.max_soft) {
        pstCh->ucSoftCount++;
        bSoftStage = true;
    } else if (pstCh->stDef.escalate_reboot) {
        bEscalate = true;
    }
    ucSoft = pstCh->ucSoftCount;
    pstCh->llLastBeatUs = llNowUs;
    portEXIT_CRITICAL(&s_xBeatLock);

    if (bSoftStage) {
        ESP_LOGW(TAG, "channel '%s': no progress for %u ms — recovery %u/%u",
                 pstCh->stDef.name, (unsigned)ulElapsedMs,
                 (unsigned)ucSoft, (unsigned)pstCh->stDef.max_soft);
        LOG_EMIT2(LOG_LEVEL_WARN, LOG_MOD_SYSTEM, LOG_EVT_WD_TIMEOUT,
                  ulId, ucSoft, pstCh->stDef.name);
        publish_wd_event(EVT_WD_TIMEOUT, ulId, (int)ulElapsedMs);
        if (pstCh->stDef.recover) {
            publish_wd_event(EVT_WD_RECOVERY_STARTED, ulId, ucSoft);
            (void)pstCh->stDef.recover(pstCh->stDef.arg);
        }
        return;
    }
    if (bEscalate) {
        escalate(ulId, pstCh);
        /* If the brake/probe blocked the reboot: keep soft-recovering. */
        if (pstCh->stDef.recover) (void)pstCh->stDef.recover(pstCh->stDef.arg);
    }
    /* warn-only channel without escalation: already re-armed above */
}

static void wd_task(void *pvArg)
{
    (void)pvArg;
    esp_task_wdt_add(NULL);            /* the supervisor is supervised */
    for (;;) {
        int64_t llNow = esp_timer_get_time();
        for (uint32_t i = 0; i < WD_MAX_CHANNELS; i++)
            if (s_astCh[i].bUsed) check_channel(i, &s_astCh[i], llNow);
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(WD_CHECK_PERIOD_MS));
    }
}

esp_err_t wd_start(void)
{
    if (s_hTask) return ESP_OK;
    if (xTaskCreatePinnedToCore(wd_task, "app_wd", 3072, NULL, 6,
                                &s_hTask, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
