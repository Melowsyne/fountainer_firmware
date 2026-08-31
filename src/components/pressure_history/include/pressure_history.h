/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "pressure_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================
 * pressure_history — firmware binding of the 1 Hz pressure ring
 * (design spec drucksensor_datenstruktur.md §21: history strictly
 * separated from the live datapoint Fon_Current_Pressure).
 *
 * Producer: pump_task (1 Hz divider in the 200 ms cycle). Consumer:
 * task_com_fill_history_batch (cloud + local maintenance access,
 * non-destructive since_seq cursor). Locking here via portMUX
 * (producer core 1 / consumer any core).
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

/* One measurement-cycle sample (the uptime timestamp is stamped internally). */
void   pressure_history_add(uint16_t pressure_mbar, uint16_t status);

/* Copy samples with seq > since_seq (oldest first); stats optional. */
size_t pressure_history_read_since(uint32_t since_seq, pressure_sample_t *out,
                                   size_t max, pressure_history_stats_t *stats);

void   pressure_history_stats_get(pressure_history_stats_t *stats);

#ifdef __cplusplus
}
#endif
