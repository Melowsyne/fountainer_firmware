/*
 * Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 *
 * This source code is proprietary. No license is granted to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of
 * this software without prior written permission from the copyright holder.
 *
 * For licensing inquiries, contact: info@melowsyne.com
 */

#pragma once

#include "pressure_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================
 * pressure_history — firmware binding of the 1 Hz pressure ring
 * (Entwurf/drucksensor_datenstruktur.md §21: history strictly
 * separated from the live datapoint Fon_Current_Pressure).
 *
 * Producer: pump_task (1 Hz divider in the 200 ms cycle). Consumer:
 * task_com_fill_history_batch (cloud + local maintenance access,
 * non-destructive since_seq cursor). Locking here via portMUX
 * (producer core 1 / consumer any).
 * ============================================================= */

/* 1 Hz -> 100 s catch-up horizon (as commissioned). Raise to 600 if
 * needed (= 10 min, 7.2 KB static) — the server poll must stay well
 * below capacity/1 Hz seconds (currently 30 s). */
#define PRESSURE_HISTORY_CAPACITY  100u
#define PRESSURE_HISTORY_INTERVAL_MS 1000u

typedef struct {
    uint32_t next_seq;
    uint32_t first_seq_available;
    uint32_t overwritten;
    uint32_t high_watermark;
    uint16_t count;
} pressure_history_stats_t;

void   pressure_history_init(void);

/* One measurement-cycle sample (uptime timestamp is stamped internally). */
void   pressure_history_add(uint16_t pressure_mbar, uint16_t status);

/* Copy samples with seq > since_seq (oldest first); stats optional. */
size_t pressure_history_read_since(uint32_t since_seq, pressure_sample_t *out,
                                   size_t max, pressure_history_stats_t *stats);

void   pressure_history_stats_get(pressure_history_stats_t *stats);

#ifdef __cplusplus
}
#endif
