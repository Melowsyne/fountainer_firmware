/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "task_measure.h"
#include "hal.h"
#include "onewire_am2302.h"
#include "data_store.h"
#include "datapoints.h"
#include "logging.h"
#include "debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "measure"

/* The pressure path (5 Hz), the pump state machine tick and the pressure
 * alerts moved to pump_task.c (work package 5) — this task keeps the SLOW
 * sensors: chip temperature (5 s) and the AM2302 climate (60 s). */

/* Small dummy/diagnostic function: prints the current values of the connected
 * hardware devices (DHT22, ADS1115 pressure, internal ESP sensor) consolidated.
 * Reads the latest state from the data_store for this (momentary output). */
static void measure_debug_dump(void)
{
    float flTemp = 0.0f, flHum = 0.0f, flBar = 0.0f, flEspTemp = 0.0f;
    data_store_float_get(DATA_TEMPERATURE,      &flTemp);
    data_store_float_get(DATA_HUMIDITY,         &flHum);
    data_store_float_get(DATA_PRESSURE,         &flBar);
    data_store_float_get(DATA_ESP_INTERNAL_TEMP, &flEspTemp);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "Sensors — DHT22: %.1f C / %.1f %%RH | pressure (ADS1115): %.2f bar | ESP core: %.1f C",
            flTemp, flHum, flBar, flEspTemp);
}

/* Climate sub-period state (persists across cycles). */
static TickType_t s_ulLastClimate;
static bool       s_bClimateDone;

/* Reads ONE AM2302 and mirrors it into its datapoints. The sensor keeps a
 * consecutive-miss counter (edge-report after 3 misses, recovery logged);
 * the LOG_EVT_CLIMATE_FAIL record carries the pin plus the driver's wiring
 * diagnosis (fail phase, line idle level) for remote debugging. */
static void climate_sensor_poll(gpio_num_t ePin, const char *pcName,
                                uint8_t *pucFails,
                                float *pflDpTemp, float *pflDpHum)
{
    float flTemperature = 0.0f, flHumidity = 0.0f;
    if (am2302_climate_read(ePin, &flTemperature, &flHumidity)) {
        if (*pucFails >= 3)
            LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_SYSTEM,
                      LOG_EVT_CLIMATE_FAIL, *pucFails,
                      1u | ((uint32_t)ePin << 8),
                      "climate sensor recovered");
        *pucFails = 0;
        *pflDpTemp = flTemperature;
        *pflDpHum  = flHumidity;
    } else {
        if (*pucFails < 255) (*pucFails)++;
        if (*pucFails == 3)
            /* b: bit0 recovered/fail, byte1 pin, byte2 handshake fail phase,
             * byte3 line idle level — enough to separate wiring faults
             * (idle 0 / phase) from a silent sensor remotely. */
            LOG_EMIT2(LOG_LEVEL_WARN, LOG_MOD_SYSTEM,
                      LOG_EVT_CLIMATE_FAIL, *pucFails,
                      0u | ((uint32_t)ePin << 8)
                         | ((uint32_t)am2302_last_fail_phase_get() << 16)
                         | ((uint32_t)am2302_last_idle_level_get() << 24),
                      "climate sensor not responding");
        if (*pucFails == 3 && am2302_last_fail_phase_get() == 2)
            /* Phase-2 post-mortem: how long the line stayed LOW past the
             * timeout (us, cap 20000) — RC vs. capacitor discrimination. */
            LOG_EMIT2(LOG_LEVEL_WARN, LOG_MOD_SYSTEM, LOG_EVT_DHT_SCAN,
                      0xFE, am2302_last_low_extra_us_get(),
                      "DHT stuck-low duration");
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "%s AM2302 (GPIO %d) read failed (%u in a row)",
                pcName, (int)ePin, (unsigned)*pucFails);
    }
}

/* TEST ONLY (wd_fault): one-shot cycle block in seconds. */
static volatile uint32_t s_ulTestBlockS;

void task_measure_test_block_set(uint32_t ulSeconds)
{
    s_ulTestBlockS = ulSeconds;
}

esp_err_t measure_task_start(tm_task_ctx_t *pstCtx)
{
    (void)pstCtx;
    s_ulLastClimate = 0;
    s_bClimateDone  = false;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "task started");
    return ESP_OK;
}

/* One 5-s device-sensing cycle: the loop, timing and stop handling are owned
 * by the task_manager component (TM_TASK_MEASURE in src/main/task_table.c). */
esp_err_t measure_task_cycle(tm_task_ctx_t *pstCtx)
{
    (void)pstCtx;
    if (s_ulTestBlockS) {                       /* injected test hang */
        uint32_t ulS = s_ulTestBlockS;
        s_ulTestBlockS = 0;
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "TEST: measure cycle blocking for %u s", (unsigned)ulS);
        vTaskDelay(pdMS_TO_TICKS(ulS * 1000));
    }
    {
        float flEspTemp = 0.0f;
        if (hal_internal_temp_read(&flEspTemp)) {
            data_store_float_set(DATA_ESP_INTERNAL_TEMP, flEspTemp);
            /* Mirror system temperature (chip) into the datapoint store. */
            DP_REF(System_Temperature) = flEspTemp;
        }

        /* System-level metrics (RSSI, flash, CPU load, uptime, heap, WLAN
         * drops) live in the MAIN monitor task now — this task is pure
         * device sensing + pump logic (see Modularisation.md). */

        /* Climate immediately on the first pass, afterwards at the climate interval. */
        TickType_t ulNow = xTaskGetTickCount();
        if (!s_bClimateDone ||
            (ulNow - s_ulLastClimate) * portTICK_PERIOD_MS >= MEASURE_CLIMATE_PERIOD_MS) {
            static uint8_t s_ucClimateFails = 0;
            climate_sensor_poll(HAL_DHT_GPIO, "climate", &s_ucClimateFails,
                                &DP_REF(Ambient_Temperature),
                                &DP_REF(Ambient_Humidity));
            /* Legacy data_store mirror (e.g. debug dump). */
            data_store_float_set(DATA_TEMPERATURE, DP_REF(Ambient_Temperature));
            data_store_float_set(DATA_HUMIDITY,    DP_REF(Ambient_Humidity));
            s_ulLastClimate = ulNow;
            s_bClimateDone = true;
        }

        measure_debug_dump();   /* print momentary values of all devices */
    }
    return ESP_OK;
}

esp_err_t measure_task_stop(tm_task_ctx_t *pstCtx)
{
    (void)pstCtx;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "task stopped");
    return ESP_OK;
}
