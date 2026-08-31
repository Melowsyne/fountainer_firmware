/* Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 * Host test mock: the DP mutex is replaced by a REAL pthread mutex so that
 * the concurrency stress test (test_datapoints.c) checks real serialization
 * — not a no-op. */
#pragma once
#include <pthread.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"

typedef pthread_mutex_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = malloc(sizeof *m);
    if (m) pthread_mutex_init(m, NULL);
    return m;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t)
{
    (void)t;
    return (h && pthread_mutex_lock(h) == 0) ? pdTRUE : pdFALSE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h)
{
    return (h && pthread_mutex_unlock(h) == 0) ? pdTRUE : pdFALSE;
}
