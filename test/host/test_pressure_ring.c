/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * Host test of the pressure sample ring (src/components/pressure_history/src/
 * pressure_ring.c, design spec drucksensor_datenstruktur.md). Pure C. Checks
 * DROP_OLDEST + seq continuity, the non-destructive read_since cursor
 * (incl. gap/empty cases), high watermark/overwritten and a randomized
 * 10k stress test against a shadow model.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pressure_ring.h"

static int s_checks;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        s_checks++;                                                       \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL(%d): %s (%s:%d)\n", s_checks, msg,      \
                    __FILE__, __LINE__);                                  \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

#define CAP 100u

static void test_basic(void)
{
    static pressure_sample_t storage[CAP];
    pressure_ring_t r;
    pressure_ring_init(&r, storage, CAP);
    CHECK(pressure_ring_count(&r) == 0 && pressure_ring_next_seq(&r) == 1, "init");
    CHECK(pressure_ring_first_seq(&r) == 1, "first==next empty");

    uint32_t seq = pressure_ring_push(&r, 2500, PRESSURE_STATUS_VALID, 1000);
    CHECK(seq == 1, "first seq");
    pressure_sample_t out[4];
    CHECK(pressure_ring_read_since(&r, 0, out, 4) == 1, "one sample");
    CHECK(out[0].sequence == 1 && out[0].pressure_mbar == 2500 &&
          out[0].status == PRESSURE_STATUS_VALID && out[0].timestamp_ms == 1000,
          "roundtrip");
    CHECK(pressure_ring_read_since(&r, 1, out, 4) == 0, "cursor at end -> empty");
}

static void test_drop_oldest(void)
{
    static pressure_sample_t storage[CAP];
    pressure_ring_t r;
    pressure_ring_init(&r, storage, CAP);

    for (uint32_t i = 0; i < 250; i++)
        pressure_ring_push(&r, (uint16_t)i, PRESSURE_STATUS_VALID, i * 1000u);

    CHECK(pressure_ring_count(&r) == CAP, "full");
    CHECK(pressure_ring_next_seq(&r) == 251, "250 pushes");
    CHECK(pressure_ring_first_seq(&r) == 151, "oldest evicted");
    CHECK(pressure_ring_overwritten(&r) == 150, "overwritten counter");
    CHECK(pressure_ring_high_watermark(&r) == CAP, "high watermark");

    /* Cursor before the ring (gap): yields from first_seq */
    static pressure_sample_t out[CAP];
    size_t n = pressure_ring_read_since(&r, 100, out, CAP);
    CHECK(n == CAP, "gap read returns full ring");
    for (size_t i = 0; i < n; i++) {
        CHECK(out[i].sequence == 151 + i, "contiguous seqs oldest-first");
        CHECK(out[i].pressure_mbar == (uint16_t)(150 + i), "values match");
    }

    /* Partial read in the middle of the ring + max bound */
    n = pressure_ring_read_since(&r, 200, out, 10);
    CHECK(n == 10 && out[0].sequence == 201 && out[9].sequence == 210,
          "partial read with max");
}

static void test_stress(void)
{
    static pressure_sample_t storage[7];   /* small capacity = lots of wrapping */
    pressure_ring_t r;
    pressure_ring_init(&r, storage, 7);

    static pressure_sample_t shadow[16384];
    uint32_t pushed = 0;
    srand(20260817);

    for (int op = 0; op < 10000; op++) {
        uint16_t mbar = (uint16_t)rand();
        uint16_t st = (uint16_t)(rand() & 0x3F);
        uint32_t ts = (uint32_t)rand();
        uint32_t seq = pressure_ring_push(&r, mbar, st, ts);
        CHECK(seq == pushed + 1, "seq monotonic gapless");
        pushed++;
        shadow[seq % 16384] = (pressure_sample_t){seq, ts, mbar, st};

        if (op % 13 == 0) {
            uint32_t since = (uint32_t)(rand() % (pushed + 2));
            pressure_sample_t out[7];
            size_t n = pressure_ring_read_since(&r, since, out, 7);
            uint32_t first = pressure_ring_first_seq(&r);
            uint32_t from = since + 1 > first ? since + 1 : first;
            size_t expect = (from >= r.next_seq) ? 0 : (size_t)(r.next_seq - from);
            if (expect > 7) expect = 7;
            CHECK(n == expect, "read count matches");
            for (size_t i = 0; i < n; i++) {
                const pressure_sample_t *s = &shadow[out[i].sequence % 16384];
                CHECK(out[i].sequence == from + i, "read seq order");
                CHECK(s->timestamp_ms == out[i].timestamp_ms &&
                      s->pressure_mbar == out[i].pressure_mbar &&
                      s->status == out[i].status, "shadow match");
            }
        }
        CHECK(pressure_ring_count(&r) <= 7, "count bound");
        CHECK(pressure_ring_first_seq(&r) == r.next_seq - r.count, "first invariant");
    }
    CHECK(pressure_ring_overwritten(&r) == pushed - 7, "overwritten total");
}

int main(void)
{
    test_basic();
    printf("1) Basic roundtrip ok\n");
    test_drop_oldest();
    printf("2) DROP_OLDEST + cursor/gaps ok\n");
    test_stress();
    printf("3) 10k stress against shadow model ok\n");
    printf("== PRESSURE_RING TEST OK (%d checks) ==\n", s_checks);
    return 0;
}
