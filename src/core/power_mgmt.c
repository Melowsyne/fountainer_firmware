/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "power_mgmt.h"
#include "datapoints.h"
#include "event_manager.h"
#include "debug.h"

#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "power"

/* DFS bounds: 160 MHz is the project's regular clock, 80 MHz the minimum
 * WiFi allows. APB stays at 80 MHz across this range, so LEDC (SSR-PWM) and
 * I2C timings are unaffected. Light sleep stays OFF on purpose: it would
 * kill the USB-Serial/JTAG console and risk WS timeouts. */
#define POWER_FREQ_MAX_MHZ 160
#define POWER_FREQ_MIN_MHZ 80

static esp_pm_lock_handle_t s_hCpuMax;         /* held -> full clock (HIGH) */
static SemaphoreHandle_t    s_hLock;           /* protects mode transitions */
static volatile int64_t     s_llLastActivityUs;
static bool                 s_bLow    = false;
static bool                 s_bInited = false;
static power_mgmt_providers_t s_stProv;        /* injected by main (DI)     */

void power_mgmt_providers_set(const power_mgmt_providers_t *pstProviders)
{
    if (pstProviders) s_stProv = *pstProviders;
}

static void mode_apply(bool bLow)
{
    if (bLow) {
        esp_pm_lock_release(s_hCpuMax);              /* DFS may go to 80 MHz */
        if (s_stProv.radio_ps_low) s_stProv.radio_ps_low(true);   /* modem sleep */
        if (s_stProv.proto_slow)   s_stProv.proto_slow(true);     /* 60 s grid   */
    } else {
        esp_pm_lock_acquire(s_hCpuMax);              /* pin 160 MHz          */
        if (s_stProv.radio_ps_low) s_stProv.radio_ps_low(false);
        if (s_stProv.proto_slow)   s_stProv.proto_slow(false);
    }
    s_bLow = bLow;
    DP_REF(System_Power_Mode) = bLow ? 1 : 0;
    {
        uint8_t ucMode = bLow ? 1 : 0;
        event_manager_publish(EVT_POWER_MODE_CHANGED, &ucMode, sizeof(ucMode));
    }
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "mode -> %s", bLow ? "LOW" : "HIGH");
}

void power_mgmt_init(void)
{
    esp_pm_config_t stCfg = {
        .max_freq_mhz = POWER_FREQ_MAX_MHZ,
        .min_freq_mhz = POWER_FREQ_MIN_MHZ,
        .light_sleep_enable = false,
    };
    esp_err_t e = esp_pm_configure(&stCfg);
    if (e != ESP_OK) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "esp_pm_configure failed (%d) — power management inactive", (int)e);
        return;
    }
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "app_high", &s_hCpuMax) != ESP_OK)
        return;
    s_hLock = xSemaphoreCreateMutex();
    if (!s_hLock) return;

    esp_pm_lock_acquire(s_hCpuMax);                  /* start in HIGH */
    s_llLastActivityUs = esp_timer_get_time();
    DP_REF(System_Power_Mode) = 0;
    s_bInited = true;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "init: DFS %d..%d MHz, LOW after %us idle",
            POWER_FREQ_MIN_MHZ, POWER_FREQ_MAX_MHZ, (unsigned)POWER_IDLE_TO_LOW_S);
}

void power_mgmt_activity_note(void)
{
    if (!s_bInited) return;
    s_llLastActivityUs = esp_timer_get_time();
    if (!s_bLow) return;
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    if (s_bLow) mode_apply(false);
    xSemaphoreGive(s_hLock);
}

void power_mgmt_tick(void)
{
    if (!s_bInited) return;

    /* "No active task": pump resting AND the server session is up (a pending
     * (re)connect wants the full clock for the TLS handshake). Probes are
     * injected by main; missing probes default to the safe answer (HIGH). */
    bool bPumpIdle = s_stProv.pump_idle  ? s_stProv.pump_idle()  : false;
    bool bSession  = s_stProv.session_up ? s_stProv.session_up() : false;

    if (s_bLow) {
        if (!bPumpIdle || !bSession) power_mgmt_activity_note();  /* -> HIGH */
        return;
    }

    int64_t llIdleUs = esp_timer_get_time() - s_llLastActivityUs;
    if (bPumpIdle && bSession && llIdleUs >= (int64_t)POWER_IDLE_TO_LOW_S * 1000000LL) {
        xSemaphoreTake(s_hLock, portMAX_DELAY);
        if (!s_bLow) mode_apply(true);
        xSemaphoreGive(s_hLock);
    }
}

bool power_mgmt_low_get(void) { return s_bLow; }
