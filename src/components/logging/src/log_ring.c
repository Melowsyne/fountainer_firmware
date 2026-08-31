/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "log_ring.h"

#include <string.h>

/* ----- LE serialization (unaligned-safe, toolchain-neutral) ------------- */

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ----- internal helpers ------------------------------------------------- */

/* A record offset is a wrap gap exactly when the marker 0 is stored there
 * (a gap always carries the marker; the exact-fit case creates no gap,
 * because head then wraps to 0 naturally). */
static bool at_wrap_gap(const log_ring_t *r, uint16_t pos)
{
    return r->buf[pos] == 0u;
}

/* Discards the oldest record (a wrap gap in front of it is released too). */
static void discard_oldest(log_ring_t *r)
{
    if (r->count == 0) return;

    if (at_wrap_gap(r, r->tail)) {
        r->used = (uint16_t)(r->used - (r->capacity - r->tail));
        r->tail = 0;
    }
    uint8_t size = r->buf[r->tail];
    r->used = (uint16_t)(r->used - size);
    r->tail = (uint16_t)(r->tail + size);
    if (r->tail == r->capacity) r->tail = 0;
    r->count--;
    r->first_seq++;
    r->overwritten++;

    if (r->count == 0) {           /* empty: normalize positions     */
        r->head = 0;
        r->tail = 0;
        r->used = 0;
    }
}

/* ----- Public API ------------------------------------------------------- */

void log_ring_init(log_ring_t *r, uint8_t *buf, uint16_t capacity_bytes)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->capacity = capacity_bytes;
    r->next_seq = 1;
    r->first_seq = 1;
}

void log_ring_clear(log_ring_t *r)
{
    r->head = 0;
    r->tail = 0;
    r->used = 0;
    r->count = 0;
    r->first_seq = r->next_seq;    /* seq keeps counting -> gap visible    */
}

uint32_t log_ring_push(log_ring_t *r, uint8_t level, uint8_t module,
                       uint16_t event_id, uint32_t uptime_ms,
                       const int32_t *args, uint8_t argc, const char *text)
{
    if (argc > LOG_RING_MAX_ARGS) argc = LOG_RING_MAX_ARGS;
    if (!args) argc = 0;

    size_t text_len = text ? strlen(text) : 0;
    uint8_t flags = argc;
    if (text_len > LOG_RING_MAX_TEXT) {
        text_len = LOG_RING_MAX_TEXT;
        flags |= LOG_RING_FLAG_TRUNCATED;
    }
    uint16_t needed = (uint16_t)(LOG_RING_HDR_SIZE + 4u * argc + text_len + 1u);

    /* Make room: first enough free globally, then contiguously at head.
     * Every iteration discards at least one record -> hard-bounded
     * (needed <= 78, min record 15: few rounds, fit for a critical section). */
    for (;;) {
        uint16_t free_bytes = (uint16_t)(r->capacity - r->used);
        if (free_bytes < needed) { discard_oldest(r); continue; }

        if (r->count == 0) { r->head = 0; r->tail = 0; }

        if (r->count == 0 || r->head >= r->tail) {
            uint16_t end_space = (uint16_t)(r->capacity - r->head);
            if (end_space >= needed) break;                  /* fits at end   */
            if ((uint16_t)(free_bytes - end_space) < needed) {
                discard_oldest(r);                            /* clear the start */
                continue;
            }
            r->buf[r->head] = 0;                              /* wrap marker  */
            r->used = (uint16_t)(r->used + end_space);
            r->head = 0;
            break;
        }
        /* head < tail: the free region [head, tail) is contiguous and
         * free_bytes >= needed was checked above. */
        break;
    }

    uint32_t seq = r->next_seq++;
    uint8_t *p = &r->buf[r->head];
    p[0] = (uint8_t)needed;
    p[1] = level;
    p[2] = module;
    p[3] = flags;
    wr_u16(&p[4], event_id);
    wr_u32(&p[6], seq);
    wr_u32(&p[10], uptime_ms);
    for (uint8_t i = 0; i < argc; i++)
        wr_u32(&p[LOG_RING_HDR_SIZE + 4u * i], (uint32_t)args[i]);
    if (text_len) memcpy(&p[LOG_RING_HDR_SIZE + 4u * argc], text, text_len);
    p[needed - 1u] = '\0';

    r->head = (uint16_t)(r->head + needed);
    if (r->head == r->capacity) r->head = 0;
    r->used = (uint16_t)(r->used + needed);
    r->count++;
    if (r->count == 1) r->first_seq = seq;
    return seq;
}

uint16_t log_ring_count(const log_ring_t *r)       { return r->count; }
uint32_t log_ring_first_seq(const log_ring_t *r)   { return r->first_seq; }
uint32_t log_ring_next_seq(const log_ring_t *r)    { return r->next_seq; }
uint32_t log_ring_overwritten(const log_ring_t *r) { return r->overwritten; }

void log_ring_iter_begin(const log_ring_t *r, log_ring_iter_t *it)
{
    it->ring = r;
    it->pos = r->tail;
    it->remaining = r->count;
}

/* Validation (design spec §33): corrupt size fields abort the iteration
 * instead of continuing with wild offsets. */
static bool record_at(const log_ring_t *r, uint16_t *pos, uint8_t *size_out)
{
    uint16_t pos_local = *pos;
    if (pos_local >= r->capacity) return false;
    if (r->buf[pos_local] == 0u) pos_local = 0;              /* wrap gap    */
    uint8_t size = r->buf[pos_local];
    if (size < LOG_RING_RECORD_MIN || size > LOG_RING_RECORD_MAX) return false;
    if ((uint32_t)pos_local + size > r->capacity) return false;
    *pos = pos_local;
    *size_out = size;
    return true;
}

bool log_ring_iter_next(log_ring_iter_t *it, log_ring_view_t *out)
{
    if (it->remaining == 0) return false;
    const log_ring_t *r = it->ring;

    uint16_t pos = it->pos;
    uint8_t size;
    if (!record_at(r, &pos, &size)) { it->remaining = 0; return false; }

    const uint8_t *p = &r->buf[pos];
    out->level     = p[1];
    out->module    = p[2];
    out->flags     = p[3];
    out->argc      = (uint8_t)(p[3] & LOG_RING_FLAG_ARGC_MASK);
    if (out->argc > LOG_RING_MAX_ARGS) out->argc = LOG_RING_MAX_ARGS;
    out->event_id  = rd_u16(&p[4]);
    out->seq       = rd_u32(&p[6]);
    out->uptime_ms = rd_u32(&p[10]);
    memset(out->args, 0, sizeof(out->args));
    for (uint8_t i = 0; i < out->argc; i++)
        out->args[i] = (int32_t)rd_u32(&p[LOG_RING_HDR_SIZE + 4u * i]);
    out->text = (const char *)&p[LOG_RING_HDR_SIZE + 4u * out->argc];

    pos = (uint16_t)(pos + size);
    if (pos == r->capacity) pos = 0;
    it->pos = pos;
    it->remaining--;
    return true;
}

void log_ring_iter_seek(log_ring_iter_t *it, uint32_t since_seq)
{
    const log_ring_t *r = it->ring;
    while (it->remaining > 0) {
        uint16_t pos = it->pos;
        uint8_t size;
        if (!record_at(r, &pos, &size)) { it->remaining = 0; return; }
        if (rd_u32(&r->buf[pos + 6]) > since_seq) { it->pos = pos; return; }
        pos = (uint16_t)(pos + size);
        if (pos == r->capacity) pos = 0;
        it->pos = pos;
        it->remaining--;
    }
}
