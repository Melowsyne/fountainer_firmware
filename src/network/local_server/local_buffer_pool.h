/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * local_buffer_pool — fixed RX frame pool of the local WSS server
 * (firmware_server.md §24-§26): network data must not cause
 * uncontrolled heap consumption; the queues carry only
 * descriptors, the payload lives here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOCAL_MAX_FRAME_SIZE  4096
#define LOCAL_FRAME_POOL_COUNT   4   /* 16 KB static (M1: RAM bottleneck) */

typedef struct {
    uint16_t length;
    uint8_t  data[LOCAL_MAX_FRAME_SIZE];
} local_frame_t;

void local_buffer_pool_init(void);

/* NULL when exhausted (overload — the caller keeps the counter). */
local_frame_t *local_buffer_acquire(void);
void           local_buffer_release(local_frame_t *frame);

/* For monitoring/tests. */
uint8_t local_buffer_in_use(void);
