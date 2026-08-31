/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================
 * pressure_ring — fixed ring buffer for 1 Hz pressure samples
 * (design spec drucksensor_datenstruktur.md). Fully generic:
 * no ESP-IDF, no FreeRTOS, no heap; synchronization is the
 * caller's responsibility.
 *
 * Deviation from the design spec (§11/§25), agreed with the user:
 * DROP_OLDEST instead of push-fail — the consumers (cloud + local
 * maintenance access) read non-destructively via a since_seq cursor
 * and never "drain" the ring. Loss is NEVER silent: the sequence
 * numbers stay gapless, `overwritten` counts every eviction and
 * first_seq_available makes the gap explicit.
 * ============================================================= */

typedef struct {
    uint32_t sequence;       /* monotonic per boot, starts at 1           */
    uint32_t timestamp_ms;   /* uptime in ms                              */
    uint16_t pressure_mbar;  /* bar * 1000, clamped to 0..65535           */
    uint16_t status;         /* PRESSURE_STATUS_* bits                    */
} pressure_sample_t;

#define PRESSURE_STATUS_VALID         0x0001u
#define PRESSURE_STATUS_SENSOR_ERROR  0x0002u
#define PRESSURE_STATUS_OUT_OF_RANGE  0x0004u
#define PRESSURE_STATUS_STALE         0x0010u  /* value = last good value  */
#define PRESSURE_STATUS_MANUAL        0x0020u  /* Fon_Pressure_Manual active */

typedef struct {
    pressure_sample_t *samples;
    uint16_t capacity;
    uint16_t head;           /* write index of the next sample            */
    uint16_t count;
    uint32_t next_seq;       /* seq of the NEXT sample (starts at 1)      */
    uint32_t overwritten;    /* evicted samples since init                */
    uint32_t high_watermark; /* max. samples buffered at the same time    */
} pressure_ring_t;

void     pressure_ring_init(pressure_ring_t *ring, pressure_sample_t *storage,
                            uint16_t capacity);

/* Writes a sample (DROP_OLDEST when the ring is full). Returns the seq. */
uint32_t pressure_ring_push(pressure_ring_t *ring, uint16_t pressure_mbar,
                            uint16_t status, uint32_t timestamp_ms);

/* Non-destructive read of all samples with seq > since_seq (oldest first,
 * at most `max` items). Seqs are gapless -> O(1) positioning. */
size_t   pressure_ring_read_since(const pressure_ring_t *ring,
                                  uint32_t since_seq,
                                  pressure_sample_t *out, size_t max);

uint16_t pressure_ring_count(const pressure_ring_t *ring);
uint32_t pressure_ring_next_seq(const pressure_ring_t *ring);
uint32_t pressure_ring_first_seq(const pressure_ring_t *ring);  /* == next when empty */
uint32_t pressure_ring_overwritten(const pressure_ring_t *ring);
uint32_t pressure_ring_high_watermark(const pressure_ring_t *ring);

#ifdef __cplusplus
}
#endif
