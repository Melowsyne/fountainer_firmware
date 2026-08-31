/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "system_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================
 * event_manager — central, decoupling distribution layer for
 * system events (see EventManager_v1.md).
 *
 *   publish() -> validate -> COPY payload into queue
 *   dispatch task -> deliver to subscribed callbacks
 *
 * It manages NO tasks and replaces neither the synchronous DI
 * probes (power_mgmt providers, link_up) nor the data_store
 * observer. Callbacks must be short, deterministic and
 * non-blocking (delegate long work to the module's own task).
 * Payloads are copied on publish (no pointer-lifetime issues);
 * subscribers get read access only and copy what they keep.
 * ============================================================= */

#define EVENT_MANAGER_QUEUE_LENGTH        16
#define EVENT_MANAGER_MAX_SUBSCRIBERS      8
#define EVENT_MANAGER_MAX_PAYLOAD_SIZE    64
#define EVENT_MANAGER_TASK_STACK_SIZE   4096
#define EVENT_MANAGER_TASK_PRIORITY        5
#define EVENT_MANAGER_QUEUE_TIMEOUT_MS    10

typedef void (*event_callback_t)(system_event_t eEvent,
                                 const void *pvData,
                                 size_t szSize);

esp_err_t event_manager_init(void);

/* Creates the dispatch task. Call after init, early in main_init. */
esp_err_t event_manager_start(void);

esp_err_t event_manager_publish(system_event_t eEvent,
                                const void *pvData,
                                size_t szSize);

esp_err_t event_manager_publish_from_isr(system_event_t eEvent,
                                         const void *pvData,
                                         size_t szSize,
                                         BaseType_t *pxHigherPrioWoken);

esp_err_t event_manager_subscribe(system_event_t eEvent, event_callback_t pfnCb);

esp_err_t event_manager_unsubscribe(system_event_t eEvent, event_callback_t pfnCb);

/* Takes ONE event from the queue (waiting up to ulTimeoutMs) and dispatches
 * it. The dispatch task loops over this; exported so host tests can drive
 * the core logic without a FreeRTOS task. Returns true if one was handled. */
bool event_manager_process_one(uint32_t ulTimeoutMs);

/* Dropped events (queue full) since boot — feeds System_Event_Drops. */
uint32_t event_manager_drops_get(void);

#ifdef __cplusplus
}
#endif
