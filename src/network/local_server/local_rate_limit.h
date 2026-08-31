/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * local_rate_limit — token bucket per local session
 * (firmware_server.md §29-§33). No dedicated task: the refill is recomputed
 * on every access from the monotonic time base (esp_timer).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LOCAL_RATE_CAPACITY        20.0f  /* Burst */
#define LOCAL_RATE_REFILL_PER_S     5.0f
#define LOCAL_RATE_STRIKE_WINDOW_US (10 * 1000 * 1000LL)
#define LOCAL_RATE_STRIKES_MAX      3     /* 3 violations in 10 s -> disconnect */

typedef struct {
    float    tokens;
    int64_t  last_refill_us;
    uint32_t violations;              /* in the current window */
    int64_t  window_start_us;
} local_rate_limit_t;

void local_rate_limit_reset(local_rate_limit_t *rl);

/* true = request allowed (cost debited). false = dropped. */
bool local_rate_allow(local_rate_limit_t *rl, float cost);

/* true = strike budget exhausted -> close the session (§33). */
bool local_rate_violation_note(local_rate_limit_t *rl);
