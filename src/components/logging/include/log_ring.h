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
 * log_ring — static byte ring with variable-length records
 * (design spec logger_datenstruktuer.md). Fully generic: no
 * ESP-IDF, no FreeRTOS, no heap — synchronization is the
 * caller's responsibility (short critical section around push/read).
 *
 * Record layout (explicitly serialized byte by byte, LE):
 *   off len field
 *   0   1   record_size  total size; 0 = wrap marker
 *   1   1   level
 *   2   1   module
 *   3   1   flags        bits 0-2 argc, bit 3 TRUNCATED
 *   4   2   event_id
 *   6   4   seq          (u32 — the wire format needs u32, not u16)
 *   10  4   uptime_ms
 *   14  4*argc args      (i32, unaligned -> memcpy)
 *   ..  N+1  text        '\0'-terminated, N <= LOG_RING_MAX_TEXT
 *
 * FIFO with DROP_OLDEST: if a new record does not fit, the oldest
 * ones give way (overwritten counts them, first_seq advances — so
 * the loss is never silent). Records are never split across the
 * end of the buffer (wrap marker record_size == 0).
 * ============================================================= */

#define LOG_RING_HDR_SIZE       14u
#define LOG_RING_MAX_TEXT       47u   /* + '\0'; fits in log_record_t.strText[48] */
#define LOG_RING_MAX_ARGS       4u
#define LOG_RING_RECORD_MIN     (LOG_RING_HDR_SIZE + 1u)
#define LOG_RING_RECORD_MAX     (LOG_RING_HDR_SIZE + 4u * LOG_RING_MAX_ARGS + \
                                 LOG_RING_MAX_TEXT + 1u)

#define LOG_RING_FLAG_ARGC_MASK 0x07u
#define LOG_RING_FLAG_TRUNCATED 0x08u

typedef struct {
    uint8_t  *buf;
    uint16_t  capacity;      /* bytes; offsets are u16 -> max 65535        */
    uint16_t  head;          /* write position (next record)               */
    uint16_t  tail;          /* offset of the oldest record                */
    uint16_t  used;          /* occupied bytes incl. wrap gap              */
    uint16_t  count;         /* valid records                              */
    uint32_t  next_seq;      /* seq of the NEXT record (starts at 1)       */
    uint32_t  first_seq;     /* seq of the oldest record; == next_seq when empty */
    uint32_t  overwritten;   /* evicted records since init                 */
} log_ring_t;

/* Decoded view of a record; text points into the ring buffer and is only
 * valid as long as the caller locks the ring against writers. */
typedef struct {
    uint8_t     level;
    uint8_t     module;
    uint8_t     flags;
    uint8_t     argc;
    uint16_t    event_id;
    uint32_t    seq;
    uint32_t    uptime_ms;
    int32_t     args[LOG_RING_MAX_ARGS];
    const char *text;
} log_ring_view_t;

typedef struct {
    const log_ring_t *ring;
    uint16_t          pos;
    uint16_t          remaining;
} log_ring_iter_t;

void     log_ring_init(log_ring_t *ring, uint8_t *buf, uint16_t capacity_bytes);

/* Discards the content but keeps next_seq (the consumer sees the gap). */
void     log_ring_clear(log_ring_t *ring);

/* Writes a record (text > LOG_RING_MAX_TEXT is truncated and flagged
 * TRUNCATED; argc is clamped). Returns the assigned seq. */
uint32_t log_ring_push(log_ring_t *ring, uint8_t level, uint8_t module,
                       uint16_t event_id, uint32_t uptime_ms,
                       const int32_t *args, uint8_t argc, const char *text);

uint16_t log_ring_count(const log_ring_t *ring);
uint32_t log_ring_first_seq(const log_ring_t *ring);
uint32_t log_ring_next_seq(const log_ring_t *ring);
uint32_t log_ring_overwritten(const log_ring_t *ring);

void     log_ring_iter_begin(const log_ring_t *ring, log_ring_iter_t *it);
bool     log_ring_iter_next(log_ring_iter_t *it, log_ring_view_t *out);

/* Fast-forwards to the first record with seq > since_seq — pure header hops
 * (record_size/seq), without decoding the skipped records. */
void     log_ring_iter_seek(log_ring_iter_t *it, uint32_t since_seq);

#ifdef __cplusplus
}
#endif
