/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "data_store.h"
#include "debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "data"
#define MAX_OBSERVERS 8

typedef struct {
    int32_t slRaw;
    float   flScale;
} entry_t;

typedef struct {
    data_key_t        eKey;
    data_observer_cb_t pfnCb;
    void              *pvCtx;
    bool               bUsed;
} observer_t;

/* Default scaling per key (0.1 => stored in tenths, etc.). */
static const float k_aflScale[DATA_KEY_COUNT] = {
    [DATA_TEMPERATURE]       = 0.1f,
    [DATA_HUMIDITY]          = 0.1f,
    [DATA_PRESSURE]          = 0.001f,  /* stored in milli-bar */
    [DATA_ESP_INTERNAL_TEMP] = 0.1f,
    [DATA_RELAY_STATE]       = 1.0f,
    [DATA_PROTOCOL_VERSION]  = 1.0f,
    [DATA_TIME_S]            = 1.0f,
    [DATA_LOGGING_ALLOW_REMOTE] = 1.0f,
};

static entry_t           s_astEntries[DATA_KEY_COUNT];
static observer_t        s_astObs[MAX_OBSERVERS];
static SemaphoreHandle_t s_hMtx;

static void notify(data_key_t eKey)
{
    /* Take a snapshot of the observers under lock, then call them outside
     * the lock (prevents deadlocks if an observer writes). */
    observer_t astLocal[MAX_OBSERVERS];
    xSemaphoreTake(s_hMtx, portMAX_DELAY);
    for (int i = 0; i < MAX_OBSERVERS; ++i) astLocal[i] = s_astObs[i];
    xSemaphoreGive(s_hMtx);

    for (int i = 0; i < MAX_OBSERVERS; ++i) {
        if (astLocal[i].bUsed && (astLocal[i].eKey == eKey || astLocal[i].eKey == DATA_KEY_ANY))
            astLocal[i].pfnCb(eKey, astLocal[i].pvCtx);
    }
}

bool data_store_init(void)
{
    s_hMtx = xSemaphoreCreateMutex();
    if (!s_hMtx) return false;
    for (int i = 0; i < DATA_KEY_COUNT; ++i) {
        s_astEntries[i].slRaw   = 0;
        s_astEntries[i].flScale = k_aflScale[i] != 0.0f ? k_aflScale[i] : 1.0f;
    }
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "init");
    return true;
}

static bool set_raw(data_key_t eKey, int32_t slRaw)
{
    if (eKey >= DATA_KEY_COUNT) return false;
    xSemaphoreTake(s_hMtx, portMAX_DELAY);
    s_astEntries[eKey].slRaw = slRaw;
    xSemaphoreGive(s_hMtx);
    notify(eKey);
    return true;
}

bool data_store_float_set(data_key_t eKey, float flValue)
{
    if (eKey >= DATA_KEY_COUNT || !s_hMtx) return false;
    int32_t slRaw = DATA_FLOAT_TO_RAW(flValue, s_astEntries[eKey].flScale);
    logging(LOG_TARGET_AUTO, DBG_LVL_VERBOSE, TAG, "set_float key=%d val=%.3f", (int)eKey, flValue);
    return set_raw(eKey, slRaw);
}

bool data_store_float_get(data_key_t eKey, float *pflOut)
{
    if (eKey >= DATA_KEY_COUNT || !pflOut || !s_hMtx) return false;
    xSemaphoreTake(s_hMtx, portMAX_DELAY);
    *pflOut = DATA_RAW_TO_FLOAT(s_astEntries[eKey].slRaw, s_astEntries[eKey].flScale);
    xSemaphoreGive(s_hMtx);
    return true;
}

bool data_store_u32_set(data_key_t eKey, uint32_t ulValue)
{
    if (eKey >= DATA_KEY_COUNT || !s_hMtx) return false;
    logging(LOG_TARGET_AUTO, DBG_LVL_VERBOSE, TAG, "set_u32 key=%d val=%u", (int)eKey, (unsigned)ulValue);
    return set_raw(eKey, (int32_t)ulValue);
}

bool data_store_u32_get(data_key_t eKey, uint32_t *pulOut)
{
    if (eKey >= DATA_KEY_COUNT || !pulOut || !s_hMtx) return false;
    xSemaphoreTake(s_hMtx, portMAX_DELAY);
    *pulOut = (uint32_t)s_astEntries[eKey].slRaw;
    xSemaphoreGive(s_hMtx);
    return true;
}

bool data_store_observer_subscribe(data_key_t eKey, data_observer_cb_t pfnCb, void *pvCtx)
{
    if (!pfnCb) return false;
    xSemaphoreTake(s_hMtx, portMAX_DELAY);
    for (int i = 0; i < MAX_OBSERVERS; ++i) {
        if (!s_astObs[i].bUsed) {
            s_astObs[i] = (observer_t){ .eKey = eKey, .pfnCb = pfnCb, .pvCtx = pvCtx, .bUsed = true };
            xSemaphoreGive(s_hMtx);
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "observer registered (key=%d)", (int)eKey);
            return true;
        }
    }
    xSemaphoreGive(s_hMtx);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "no observer slot free");
    return false;
}
