/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "pressure_history.h"

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

static pressure_sample_t s_astSamples[PRESSURE_HISTORY_CAPACITY];
static pressure_ring_t   s_stRing;

/* Producer (pump_task, core 1) and consumer (task_com/httpd worker) run on
 * different cores -> short portMUX lock as with the log ring. */
static portMUX_TYPE s_xLock = portMUX_INITIALIZER_UNLOCKED;

void pressure_history_init(void)
{
    pressure_ring_init(&s_stRing, s_astSamples, PRESSURE_HISTORY_CAPACITY);
}

void pressure_history_add(uint16_t usMbar, uint16_t usStatus)
{
    uint32_t ulNowMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&s_xLock);
    pressure_ring_push(&s_stRing, usMbar, usStatus, ulNowMs);
    portEXIT_CRITICAL(&s_xLock);
}

static void stats_fill_locked(pressure_history_stats_t *pstStats)
{
    pstStats->next_seq            = pressure_ring_next_seq(&s_stRing);
    pstStats->first_seq_available = pressure_ring_first_seq(&s_stRing);
    pstStats->overwritten         = pressure_ring_overwritten(&s_stRing);
    pstStats->high_watermark      = pressure_ring_high_watermark(&s_stRing);
    pstStats->count               = pressure_ring_count(&s_stRing);
}

size_t pressure_history_read_since(uint32_t ulSinceSeq, pressure_sample_t *pstOut,
                                   size_t szMax, pressure_history_stats_t *pstStats)
{
    portENTER_CRITICAL(&s_xLock);
    size_t szN = pressure_ring_read_since(&s_stRing, ulSinceSeq, pstOut, szMax);
    if (pstStats) stats_fill_locked(pstStats);
    portEXIT_CRITICAL(&s_xLock);
    return szN;
}

void pressure_history_stats_get(pressure_history_stats_t *pstStats)
{
    if (!pstStats) return;
    portENTER_CRITICAL(&s_xLock);
    stats_fill_locked(pstStats);
    portEXIT_CRITICAL(&s_xLock);
}
