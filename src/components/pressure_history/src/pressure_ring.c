/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "pressure_ring.h"

#include <string.h>

void pressure_ring_init(pressure_ring_t *r, pressure_sample_t *storage,
                        uint16_t capacity)
{
    memset(r, 0, sizeof(*r));
    r->samples = storage;
    r->capacity = capacity;
    r->next_seq = 1;
}

uint32_t pressure_ring_push(pressure_ring_t *r, uint16_t pressure_mbar,
                            uint16_t status, uint32_t timestamp_ms)
{
    uint32_t seq = r->next_seq++;
    pressure_sample_t *s = &r->samples[r->head];
    s->sequence      = seq;
    s->timestamp_ms  = timestamp_ms;
    s->pressure_mbar = pressure_mbar;
    s->status        = status;

    r->head = (uint16_t)((r->head + 1u) % r->capacity);
    if (r->count < r->capacity) {
        r->count++;
        if (r->count > r->high_watermark) r->high_watermark = r->count;
    } else {
        r->overwritten++;          /* oldest sample evicted (DROP_OLDEST)       */
    }
    return seq;
}

size_t pressure_ring_read_since(const pressure_ring_t *r, uint32_t since_seq,
                                pressure_sample_t *out, size_t max)
{
    if (!out || max == 0 || r->count == 0) return 0;

    uint32_t first = r->next_seq - r->count;        /* oldest available seq     */
    uint32_t from  = (since_seq + 1u > first) ? since_seq + 1u : first;
    if (from >= r->next_seq) return 0;

    uint32_t avail = r->next_seq - from;
    size_t n = (avail < max) ? avail : max;

    /* Index of the sample with seq `from`: the oldest is at head - count. */
    uint32_t oldest_idx = (uint32_t)(r->head + r->capacity - r->count) % r->capacity;
    uint32_t idx = (oldest_idx + (from - first)) % r->capacity;
    for (size_t i = 0; i < n; i++) {
        out[i] = r->samples[idx];
        idx = (idx + 1u) % r->capacity;
    }
    return n;
}

uint16_t pressure_ring_count(const pressure_ring_t *r)          { return r->count; }
uint32_t pressure_ring_next_seq(const pressure_ring_t *r)       { return r->next_seq; }
uint32_t pressure_ring_first_seq(const pressure_ring_t *r)      { return r->next_seq - r->count; }
uint32_t pressure_ring_overwritten(const pressure_ring_t *r)    { return r->overwritten; }
uint32_t pressure_ring_high_watermark(const pressure_ring_t *r) { return r->high_watermark; }
