/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */
#include "local_rate_limit.h"

#include "esp_timer.h"

void local_rate_limit_reset(local_rate_limit_t *rl)
{
    rl->tokens = LOCAL_RATE_CAPACITY;
    rl->last_refill_us = esp_timer_get_time();
    rl->violations = 0;
    rl->window_start_us = 0;
}

bool local_rate_allow(local_rate_limit_t *rl, float cost)
{
    const int64_t llNow = esp_timer_get_time();
    const float flDelta = (float)(llNow - rl->last_refill_us) / 1e6f;
    rl->last_refill_us = llNow;
    rl->tokens += flDelta * LOCAL_RATE_REFILL_PER_S;
    if (rl->tokens > LOCAL_RATE_CAPACITY) rl->tokens = LOCAL_RATE_CAPACITY;
    if (rl->tokens < cost) return false;
    rl->tokens -= cost;
    return true;
}

bool local_rate_violation_note(local_rate_limit_t *rl)
{
    const int64_t llNow = esp_timer_get_time();
    if (rl->window_start_us == 0 ||
        llNow - rl->window_start_us > LOCAL_RATE_STRIKE_WINDOW_US) {
        rl->window_start_us = llNow;
        rl->violations = 0;
    }
    return ++rl->violations >= LOCAL_RATE_STRIKES_MAX;
}
