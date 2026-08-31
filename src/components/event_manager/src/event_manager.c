/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "event_manager.h"

#include <string.h>
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "event_mgr";

typedef struct {
    system_event_t eType;
    uint8_t        aucData[EVENT_MANAGER_MAX_PAYLOAD_SIZE];
    size_t         szSize;
    uint32_t       ulTimestampMs;
} event_message_t;

typedef struct {
    event_callback_t apfnCb[EVENT_MANAGER_MAX_SUBSCRIBERS];
    uint8_t          ucCount;
} subscriber_list_t;

static QueueHandle_t     s_hQueue;
static SemaphoreHandle_t s_hSubMutex;
static TaskHandle_t      s_hTask;
static subscriber_list_t s_astSubs[EVENT_MAX];
static bool              s_bInited;
static uint32_t          s_ulDrops;          /* queue-full drops since boot */

static bool event_valid(system_event_t eEvent)
{
    return eEvent > EVENT_NONE && eEvent < EVENT_MAX;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Deliver to the subscribers. The callback list is copied under the mutex,
 * the callbacks run WITHOUT it (a callback may subscribe/unsubscribe). */
static void dispatch(const event_message_t *pstMsg)
{
    event_callback_t apfnCopy[EVENT_MANAGER_MAX_SUBSCRIBERS];
    uint8_t ucCount = 0;

    if (!pstMsg || !event_valid(pstMsg->eType)) return;

    if (xSemaphoreTake(s_hSubMutex, portMAX_DELAY) == pdTRUE) {
        const subscriber_list_t *pstList = &s_astSubs[pstMsg->eType];
        ucCount = pstList->ucCount;
        for (uint8_t i = 0; i < ucCount; ++i) apfnCopy[i] = pstList->apfnCb[i];
        xSemaphoreGive(s_hSubMutex);
    }

    for (uint8_t i = 0; i < ucCount; ++i) {
        if (apfnCopy[i])
            apfnCopy[i](pstMsg->eType, pstMsg->szSize ? pstMsg->aucData : NULL,
                        pstMsg->szSize);
    }
}

bool event_manager_process_one(uint32_t ulTimeoutMs)
{
    event_message_t stMsg;
    if (!s_hQueue) return false;
    if (xQueueReceive(s_hQueue, &stMsg, pdMS_TO_TICKS(ulTimeoutMs)) != pdTRUE)
        return false;
    dispatch(&stMsg);
    return true;
}

static void event_manager_task(void *pvArg)
{
    (void)pvArg;
    for (;;) (void)event_manager_process_one(portMAX_DELAY);
}

esp_err_t event_manager_init(void)
{
    if (s_bInited) return ESP_OK;

    memset(s_astSubs, 0, sizeof(s_astSubs));

    s_hQueue = xQueueCreate(EVENT_MANAGER_QUEUE_LENGTH, sizeof(event_message_t));
    if (!s_hQueue) return ESP_ERR_NO_MEM;

    s_hSubMutex = xSemaphoreCreateMutex();
    if (!s_hSubMutex) {
        vQueueDelete(s_hQueue);
        s_hQueue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_bInited = true;
    return ESP_OK;
}

esp_err_t event_manager_start(void)
{
    if (!s_bInited) return ESP_ERR_INVALID_STATE;
    if (s_hTask) return ESP_OK;

    if (xTaskCreate(event_manager_task, "event_mgr",
                    EVENT_MANAGER_TASK_STACK_SIZE, NULL,
                    EVENT_MANAGER_TASK_PRIORITY, &s_hTask) != pdPASS) {
        s_hTask = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Fill + validate a message (shared by the task- and ISR-publish paths). */
static esp_err_t message_prepare(event_message_t *pstMsg, system_event_t eEvent,
                                 const void *pvData, size_t szSize)
{
    if (!s_bInited || !s_hQueue)          return ESP_ERR_INVALID_STATE;
    if (!event_valid(eEvent))             return ESP_ERR_INVALID_ARG;
    if (szSize > EVENT_MANAGER_MAX_PAYLOAD_SIZE) return ESP_ERR_INVALID_SIZE;
    if (szSize > 0 && !pvData)            return ESP_ERR_INVALID_ARG;

    memset(pstMsg, 0, sizeof(*pstMsg));
    pstMsg->eType = eEvent;
    pstMsg->szSize = szSize;
    pstMsg->ulTimestampMs = now_ms();
    if (szSize) memcpy(pstMsg->aucData, pvData, szSize);
    return ESP_OK;
}

esp_err_t event_manager_publish(system_event_t eEvent,
                                const void *pvData, size_t szSize)
{
    event_message_t stMsg;
    esp_err_t e = message_prepare(&stMsg, eEvent, pvData, szSize);
    if (e != ESP_OK) return e;

    if (xQueueSend(s_hQueue, &stMsg,
                   pdMS_TO_TICKS(EVENT_MANAGER_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        s_ulDrops++;
        ESP_LOGW(TAG, "queue full, event=%d dropped", (int)eEvent);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t event_manager_publish_from_isr(system_event_t eEvent,
                                         const void *pvData, size_t szSize,
                                         BaseType_t *pxHigherPrioWoken)
{
    event_message_t stMsg;
    esp_err_t e = message_prepare(&stMsg, eEvent, pvData, szSize);
    if (e != ESP_OK) return e;

    if (xQueueSendFromISR(s_hQueue, &stMsg, pxHigherPrioWoken) != pdTRUE) {
        s_ulDrops++;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t event_manager_subscribe(system_event_t eEvent, event_callback_t pfnCb)
{
    if (!s_bInited || !s_hSubMutex)    return ESP_ERR_INVALID_STATE;
    if (!event_valid(eEvent) || !pfnCb) return ESP_ERR_INVALID_ARG;

    esp_err_t e = ESP_OK;
    xSemaphoreTake(s_hSubMutex, portMAX_DELAY);
    subscriber_list_t *pstList = &s_astSubs[eEvent];

    for (uint8_t i = 0; i < pstList->ucCount; ++i) {
        if (pstList->apfnCb[i] == pfnCb) {         /* already subscribed */
            xSemaphoreGive(s_hSubMutex);
            return ESP_OK;
        }
    }
    if (pstList->ucCount >= EVENT_MANAGER_MAX_SUBSCRIBERS) {
        e = ESP_ERR_NO_MEM;
    } else {
        pstList->apfnCb[pstList->ucCount++] = pfnCb;
    }
    xSemaphoreGive(s_hSubMutex);
    return e;
}

esp_err_t event_manager_unsubscribe(system_event_t eEvent, event_callback_t pfnCb)
{
    if (!s_bInited || !s_hSubMutex)    return ESP_ERR_INVALID_STATE;
    if (!event_valid(eEvent) || !pfnCb) return ESP_ERR_INVALID_ARG;

    esp_err_t e = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_hSubMutex, portMAX_DELAY);
    subscriber_list_t *pstList = &s_astSubs[eEvent];

    for (uint8_t i = 0; i < pstList->ucCount; ++i) {
        if (pstList->apfnCb[i] == pfnCb) {
            for (uint8_t j = i; j + 1 < pstList->ucCount; ++j)
                pstList->apfnCb[j] = pstList->apfnCb[j + 1];
            pstList->apfnCb[--pstList->ucCount] = NULL;
            e = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_hSubMutex);
    return e;
}

uint32_t event_manager_drops_get(void) { return s_ulDrops; }
