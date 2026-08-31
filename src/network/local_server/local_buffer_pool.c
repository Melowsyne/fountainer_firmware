/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */
#include "local_buffer_pool.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static local_frame_t     s_astPool[LOCAL_FRAME_POOL_COUNT];
static bool              s_abUsed[LOCAL_FRAME_POOL_COUNT];
static SemaphoreHandle_t s_hLock;

void local_buffer_pool_init(void)
{
    if (!s_hLock) s_hLock = xSemaphoreCreateMutex();
    memset(s_abUsed, 0, sizeof s_abUsed);
}

local_frame_t *local_buffer_acquire(void)
{
    local_frame_t *pstFrame = NULL;
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    for (int i = 0; i < LOCAL_FRAME_POOL_COUNT; i++) {
        if (!s_abUsed[i]) {
            s_abUsed[i] = true;
            pstFrame = &s_astPool[i];
            break;
        }
    }
    xSemaphoreGive(s_hLock);
    return pstFrame;
}

void local_buffer_release(local_frame_t *frame)
{
    if (!frame) return;
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    s_abUsed[frame - s_astPool] = false;
    xSemaphoreGive(s_hLock);
}

uint8_t local_buffer_in_use(void)
{
    uint8_t ucN = 0;
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    for (int i = 0; i < LOCAL_FRAME_POOL_COUNT; i++)
        if (s_abUsed[i]) ucN++;
    xSemaphoreGive(s_hLock);
    return ucN;
}
