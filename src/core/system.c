/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "system.h"
#include "debug.h"

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#define TAG "system"

static void (*s_pfnPreReboot)(void);
static uint32_t s_ulDelayMs;

void system_pre_reboot_hook_set(void (*pfnHook)(void)) { s_pfnPreReboot = pfnHook; }

/* esp_restart() does not return — it must never run in a caller's context
 * (e.g. the WS session callback still has a command_result to send). */
static void reboot_task(void *pvArg)
{
    (void)pvArg;
    vTaskDelay(pdMS_TO_TICKS(s_ulDelayMs));
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "rebooting now");
    if (s_pfnPreReboot) s_pfnPreReboot();     /* clean teardown (injected) */
    esp_restart();                            /* does not return */
}

bool system_reboot_deferred(uint32_t ulDelayMs)
{
    s_ulDelayMs = ulDelayMs;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "deferred reboot in %u ms",
            (unsigned)ulDelayMs);
    return xTaskCreate(reboot_task, "reboot", 4096, NULL, 6, NULL) == pdPASS;
}
