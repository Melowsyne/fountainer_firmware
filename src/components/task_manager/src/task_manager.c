/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "task_manager.h"

#include <string.h>
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "event_manager.h"

static const char *TAG = "task_mgr";

struct tm_task_ctx {
    tm_task_id_t ulId;
    void *pvUserArg;
};

typedef struct {
    bool bUsed;
    char strName[configMAX_TASK_NAME_LEN];

    tm_task_callback_t pfnStart, pfnCycle, pfnStop;
    void *pvUserArg;

    uint32_t ulPeriodMs;
    TickType_t xPeriodTicks;
    uint32_t ulMaxRuntimeMs;
    uint32_t ulStackSize;
    UBaseType_t uxPriority;
    BaseType_t xCoreId;
    bool bTwdt;

    TaskHandle_t hTask;
    SemaphoreHandle_t hFinished;
    volatile bool bStopRequested;
    tm_task_state_t eState;

    volatile uint32_t ulLastCycleMs;   /* runtime of the last on_cycle       */
    bool bStackWarned;                 /* edge trigger for EVT_TASK_STACK_LOW */
} tm_slot_t;

static tm_slot_t s_astSlots[TASK_MANAGER_MAX_TASKS];
static SemaphoreHandle_t s_hLock;

static bool id_valid(tm_task_id_t ulId) { return ulId < TASK_MANAGER_MAX_TASKS; }

static void publish_task_event(system_event_t eEvent, tm_task_id_t ulId, int slReason)
{
    evt_task_info_t stInfo = { .ucTaskId = (unsigned char)ulId, .slReason = slReason };
    event_manager_publish(eEvent, &stInfo, sizeof(stInfo));
}

/* Owns the loop, the fixed time grid, supervision and cooperative stop. */
static void tm_task_wrapper(void *pvParam)
{
    tm_task_id_t ulId = (tm_task_id_t)(uintptr_t)pvParam;
    tm_slot_t *pstSlot = &s_astSlots[ulId];

    tm_task_ctx_t stCtx = { .ulId = ulId, .pvUserArg = pstSlot->pvUserArg };

    xSemaphoreTake(s_hLock, portMAX_DELAY);
    pstSlot->eState = TM_STATE_RUNNING;
    xSemaphoreGive(s_hLock);

    if (pstSlot->bTwdt) esp_task_wdt_add(NULL);
    if (pstSlot->pfnStart) (void)pstSlot->pfnStart(&stCtx);
    publish_task_event(EVT_TASK_STARTED, ulId, 0);

    TickType_t xLastWake = xTaskGetTickCount();

    while (!pstSlot->bStopRequested) {
        int64_t llT0 = esp_timer_get_time();
        if (pstSlot->pfnCycle) {
            esp_err_t err = pstSlot->pfnCycle(&stCtx);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "task '%s' cycle returned %s",
                         pstSlot->strName, esp_err_to_name(err));
        }
        uint32_t ulDtMs = (uint32_t)((esp_timer_get_time() - llT0) / 1000);
        pstSlot->ulLastCycleMs = ulDtMs;

        /* Supervision: per-cycle runtime budget + own stack watermark. */
        if (pstSlot->ulMaxRuntimeMs && ulDtMs > pstSlot->ulMaxRuntimeMs) {
            ESP_LOGW(TAG, "task '%s' cycle overrun: %u ms (budget %u ms)",
                     pstSlot->strName, (unsigned)ulDtMs,
                     (unsigned)pstSlot->ulMaxRuntimeMs);
            publish_task_event(EVT_TASK_CYCLE_OVERRUN, ulId, (int)ulDtMs);
        }
        UBaseType_t uxFree = uxTaskGetStackHighWaterMark(NULL);
        if (uxFree < TM_STACK_LOW_WATERMARK_WORDS && !pstSlot->bStackWarned) {
            ESP_LOGW(TAG, "task '%s' stack low: %u words free",
                     pstSlot->strName, (unsigned)uxFree);
            publish_task_event(EVT_TASK_STACK_LOW, ulId, (int)uxFree);
            pstSlot->bStackWarned = true;
        }

        /* TWDT only AFTER a completed cycle (never feed it blindly). */
        if (pstSlot->bTwdt) esp_task_wdt_reset();

        xTaskDelayUntil(&xLastWake, pstSlot->xPeriodTicks);
    }

    if (pstSlot->pfnStop) (void)pstSlot->pfnStop(&stCtx);
    if (pstSlot->bTwdt) esp_task_wdt_delete(NULL);
    publish_task_event(EVT_TASK_STOPPED, ulId, 0);

    xSemaphoreTake(s_hLock, portMAX_DELAY);
    pstSlot->hTask = NULL;
    pstSlot->bStopRequested = false;
    pstSlot->eState = TM_STATE_STOPPED;
    xSemaphoreGive(s_hLock);

    xSemaphoreGive(pstSlot->hFinished);
    vTaskDelete(NULL);
}

esp_err_t tm_init(void)
{
    if (s_hLock) return ESP_ERR_INVALID_STATE;
    memset(s_astSlots, 0, sizeof(s_astSlots));
    s_hLock = xSemaphoreCreateMutex();
    return s_hLock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t tm_register_periodic(tm_task_id_t ulId, const tm_periodic_task_def_t *pstDef)
{
    if (!id_valid(ulId))                                   return ESP_ERR_INVALID_ARG;
    if (!pstDef || !pstDef->name || !pstDef->on_cycle)     return ESP_ERR_INVALID_ARG;
    if (pstDef->period_ms == 0 || pstDef->stack_size == 0) return ESP_ERR_INVALID_ARG;
    if (!s_hLock)                                          return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_hLock, portMAX_DELAY);
    tm_slot_t *pstSlot = &s_astSlots[ulId];
    if (pstSlot->bUsed) {
        xSemaphoreGive(s_hLock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(pstSlot, 0, sizeof(*pstSlot));

    pstSlot->hFinished = xSemaphoreCreateBinary();
    if (!pstSlot->hFinished) {
        xSemaphoreGive(s_hLock);
        return ESP_ERR_NO_MEM;
    }

    pstSlot->bUsed         = true;
    pstSlot->pfnStart      = pstDef->on_start;
    pstSlot->pfnCycle      = pstDef->on_cycle;
    pstSlot->pfnStop       = pstDef->on_stop;
    pstSlot->pvUserArg     = pstDef->user_arg;
    pstSlot->ulPeriodMs    = pstDef->period_ms;
    pstSlot->xPeriodTicks  = pdMS_TO_TICKS(pstDef->period_ms);
    if (pstSlot->xPeriodTicks == 0) pstSlot->xPeriodTicks = 1;
    pstSlot->ulMaxRuntimeMs = pstDef->max_runtime_ms;
    pstSlot->ulStackSize   = pstDef->stack_size;
    pstSlot->uxPriority    = pstDef->priority;
    pstSlot->xCoreId       = pstDef->core_id;
    pstSlot->bTwdt         = pstDef->twdt_subscribe;
    pstSlot->eState        = TM_STATE_REGISTERED;
    strlcpy(pstSlot->strName, pstDef->name, sizeof(pstSlot->strName));

    xSemaphoreGive(s_hLock);
    return ESP_OK;
}

esp_err_t tm_start(tm_task_id_t ulId)
{
    if (!id_valid(ulId)) return ESP_ERR_INVALID_ARG;
    if (!s_hLock)        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_hLock, portMAX_DELAY);
    tm_slot_t *pstSlot = &s_astSlots[ulId];
    if (!pstSlot->bUsed || pstSlot->hTask) {
        xSemaphoreGive(s_hLock);
        return ESP_ERR_INVALID_STATE;
    }
    pstSlot->bStopRequested = false;
    while (xSemaphoreTake(pstSlot->hFinished, 0) == pdTRUE) { /* drain */ }

    BaseType_t xOk = xTaskCreatePinnedToCore(tm_task_wrapper, pstSlot->strName,
                                             pstSlot->ulStackSize,
                                             (void *)(uintptr_t)ulId,
                                             pstSlot->uxPriority,
                                             &pstSlot->hTask, pstSlot->xCoreId);
    if (xOk != pdPASS) {
        pstSlot->hTask = NULL;
        pstSlot->eState = TM_STATE_STOPPED;
        xSemaphoreGive(s_hLock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_hLock);
    ESP_LOGI(TAG, "task '%s' started (period %u ms)", pstSlot->strName,
             (unsigned)pstSlot->ulPeriodMs);
    return ESP_OK;
}

esp_err_t tm_stop(tm_task_id_t ulId, TickType_t xTimeout)
{
    if (!id_valid(ulId)) return ESP_ERR_INVALID_ARG;
    if (!s_hLock)        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_hLock, portMAX_DELAY);
    tm_slot_t *pstSlot = &s_astSlots[ulId];
    if (!pstSlot->bUsed) {
        xSemaphoreGive(s_hLock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!pstSlot->hTask) {
        xSemaphoreGive(s_hLock);
        return ESP_OK;
    }
    pstSlot->eState = TM_STATE_STOPPING;
    pstSlot->bStopRequested = true;
    TaskHandle_t hTask = pstSlot->hTask;
    SemaphoreHandle_t hFinished = pstSlot->hFinished;
    xSemaphoreGive(s_hLock);

    xTaskNotify(hTask, 0, eNoAction);
    if (xSemaphoreTake(hFinished, xTimeout) != pdTRUE) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

/* start_all/stop_all iterate over the REGISTERED slots (the project table
 * defines which IDs exist; unused slots are skipped). */
esp_err_t tm_start_all(void)
{
    for (tm_task_id_t id = 0; id < TASK_MANAGER_MAX_TASKS; id++) {
        if (!s_astSlots[id].bUsed) continue;
        esp_err_t err = tm_start(id);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t tm_stop_all(TickType_t xTimeoutPerTask)
{
    for (tm_task_id_t id = 0; id < TASK_MANAGER_MAX_TASKS; id++) {
        if (!s_astSlots[id].bUsed) continue;
        esp_err_t err = tm_stop(id, xTimeoutPerTask);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

bool tm_should_stop(tm_task_ctx_t *pstCtx)
{
    if (!pstCtx || !id_valid(pstCtx->ulId)) return true;
    return s_astSlots[pstCtx->ulId].bStopRequested;
}

void *tm_get_arg(tm_task_ctx_t *pstCtx) { return pstCtx ? pstCtx->pvUserArg : NULL; }

tm_task_id_t tm_get_id(tm_task_ctx_t *pstCtx)
{
    return pstCtx ? pstCtx->ulId : (tm_task_id_t)TASK_MANAGER_MAX_TASKS;
}

TaskHandle_t tm_get_handle(tm_task_id_t ulId)
{
    return id_valid(ulId) ? s_astSlots[ulId].hTask : NULL;
}

tm_task_state_t tm_get_state(tm_task_id_t ulId)
{
    return id_valid(ulId) ? s_astSlots[ulId].eState : TM_STATE_UNUSED;
}

uint32_t tm_get_last_cycle_ms(tm_task_id_t ulId)
{
    return id_valid(ulId) ? s_astSlots[ulId].ulLastCycleMs : 0;
}

UBaseType_t tm_get_stack_watermark(tm_task_id_t ulId)
{
    if (!id_valid(ulId) || !s_astSlots[ulId].hTask) return 0;
    return uxTaskGetStackHighWaterMark(s_astSlots[ulId].hTask);
}
