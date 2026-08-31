/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * Host test of the generic byte ring (src/components/logging/src/log_ring.c,
 * design spec logger_datenstruktuer.md). Pure C — no mock needed. Checks the
 * round trip of all fields, truncation, wrap markers at all offsets (text-length
 * sweep), eviction invariants, iter_seek, clear-keeps-seq and a randomized
 * 100k stress test against a shadow model.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log_ring.h"

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

/* ----- Invariants, checkable after every operation ---------------------- */
static void check_invariants(const log_ring_t *r)
{
    CHECK(r->used <= r->capacity, "used <= capacity");
    CHECK(r->first_seq == r->next_seq - r->count, "first_seq == next_seq - count");

    /* Iterator yields exactly count records with ascending, gapless seqs. */
    log_ring_iter_t it;
    log_ring_view_t v;
    log_ring_iter_begin(r, &it);
    uint32_t n = 0, expect = r->first_seq;
    while (log_ring_iter_next(&it, &v)) {
        CHECK(v.seq == expect, "seq contiguous");
        CHECK(v.text[strlen(v.text)] == '\0', "text NUL-terminated");
        CHECK(strlen(v.text) <= LOG_RING_MAX_TEXT, "text length bound");
        expect++;
        n++;
    }
    CHECK(n == r->count, "iterator yields count records");
}

/* ----- 1) Round trip of all fields -------------------------------------- */
static void test_roundtrip(void)
{
    static uint8_t buf[256];
    log_ring_t r;
    log_ring_init(&r, buf, sizeof buf);
    CHECK(log_ring_count(&r) == 0 && log_ring_next_seq(&r) == 1, "init state");

    int32_t args[4] = { -1, 0x7FFFFFFF, -2147483647 - 1, 42 };
    uint32_t seq = log_ring_push(&r, 3, 7, 0xBEEF, 123456789u, args, 4, "hello");
    CHECK(seq == 1, "first seq is 1");

    log_ring_iter_t it; log_ring_view_t v;
    log_ring_iter_begin(&r, &it);
    CHECK(log_ring_iter_next(&it, &v), "one record");
    CHECK(v.level == 3 && v.module == 7 && v.event_id == 0xBEEF, "meta roundtrip");
    CHECK(v.uptime_ms == 123456789u && v.seq == 1, "seq/uptime roundtrip");
    CHECK(v.argc == 4, "argc");
    for (int i = 0; i < 4; i++) CHECK(v.args[i] == args[i], "arg roundtrip");
    CHECK(strcmp(v.text, "hello") == 0, "text roundtrip");
    CHECK(!(v.flags & LOG_RING_FLAG_TRUNCATED), "not truncated");
    CHECK(!log_ring_iter_next(&it, &v), "iterator exhausted");

    /* argc 0..4, empty text, NULL text */
    for (uint8_t a = 0; a <= 4; a++) {
        log_ring_push(&r, 1, 1, a, 0, args, a, a % 2 ? "" : NULL);
    }
    check_invariants(&r);
}

/* ----- 2) Truncation ---------------------------------------------------- */
static void test_truncation(void)
{
    static uint8_t buf[256];
    log_ring_t r;
    log_ring_init(&r, buf, sizeof buf);

    char longtext[LOG_RING_MAX_TEXT + 20];
    memset(longtext, 'x', sizeof longtext - 1);
    longtext[sizeof longtext - 1] = '\0';
    log_ring_push(&r, 1, 1, 1, 0, NULL, 0, longtext);

    log_ring_iter_t it; log_ring_view_t v;
    log_ring_iter_begin(&r, &it);
    CHECK(log_ring_iter_next(&it, &v), "record present");
    CHECK(strlen(v.text) == LOG_RING_MAX_TEXT, "truncated to max");
    CHECK(v.flags & LOG_RING_FLAG_TRUNCATED, "truncation flag set");

    /* exactly maximum length: NO flag */
    char exact[LOG_RING_MAX_TEXT + 1];
    memset(exact, 'y', LOG_RING_MAX_TEXT);
    exact[LOG_RING_MAX_TEXT] = '\0';
    log_ring_push(&r, 1, 1, 2, 0, NULL, 0, exact);
    log_ring_iter_begin(&r, &it);
    log_ring_iter_next(&it, &v);
    CHECK(log_ring_iter_next(&it, &v), "second record");
    CHECK(!(v.flags & LOG_RING_FLAG_TRUNCATED) &&
          strlen(v.text) == LOG_RING_MAX_TEXT, "exact length untruncated");
    check_invariants(&r);
}

/* ----- 3) Wrap marker sweep --------------------------------------------- */
/* Small ring + all text lengths 0..47: this makes the wrap land on every
 * possible offset, incl. exact fit (no marker) and a 1-byte residual gap. */
static void test_wrap_sweep(void)
{
    for (size_t tl = 0; tl <= LOG_RING_MAX_TEXT; tl++) {
        static uint8_t buf[128];
        log_ring_t r;
        log_ring_init(&r, buf, sizeof buf);

        char text[LOG_RING_MAX_TEXT + 1];
        memset(text, 'a', tl);
        text[tl] = '\0';

        for (int i = 0; i < 500; i++) {
            log_ring_push(&r, 1, 2, (uint16_t)i, (uint32_t)i, NULL, 0, text);
            check_invariants(&r);
        }
        CHECK(log_ring_next_seq(&r) == 501, "500 pushes counted");
        CHECK(log_ring_overwritten(&r) == 500u - log_ring_count(&r),
              "overwritten accounts evictions");
    }
}

/* ----- 4) iter_seek ------------------------------------------------------ */
static void test_seek(void)
{
    static uint8_t buf[1024];
    log_ring_t r;
    log_ring_init(&r, buf, sizeof buf);
    for (int i = 0; i < 200; i++)
        log_ring_push(&r, 1, 1, 0, 0, NULL, 0, "seekme");

    uint32_t first = log_ring_first_seq(&r);
    uint32_t next  = log_ring_next_seq(&r);

    /* into the middle */
    uint32_t since = first + 5;
    log_ring_iter_t it; log_ring_view_t v;
    log_ring_iter_begin(&r, &it);
    log_ring_iter_seek(&it, since);
    CHECK(log_ring_iter_next(&it, &v) && v.seq == since + 1, "seek lands after since");

    /* before the ring (overflow case): yields from first_seq */
    log_ring_iter_begin(&r, &it);
    log_ring_iter_seek(&it, 0);
    CHECK(log_ring_iter_next(&it, &v) && v.seq == first, "seek 0 -> oldest");

    /* past the ring: empty */
    log_ring_iter_begin(&r, &it);
    log_ring_iter_seek(&it, next);
    CHECK(!log_ring_iter_next(&it, &v), "seek past end -> empty");
}

/* ----- 5) clear keeps seq ----------------------------------------------- */
static void test_clear(void)
{
    static uint8_t buf[256];
    log_ring_t r;
    log_ring_init(&r, buf, sizeof buf);
    for (int i = 0; i < 5; i++) log_ring_push(&r, 1, 1, 0, 0, NULL, 0, "x");
    uint32_t next = log_ring_next_seq(&r);
    log_ring_clear(&r);
    CHECK(log_ring_count(&r) == 0, "cleared");
    CHECK(log_ring_next_seq(&r) == next, "seq keeps counting");
    CHECK(log_ring_first_seq(&r) == next, "first == next when empty");
    uint32_t seq = log_ring_push(&r, 1, 1, 0, 0, NULL, 0, "y");
    CHECK(seq == next, "gapless continuation after clear");
    check_invariants(&r);
}

/* ----- 6) randomized stress test against shadow model ------------------- */
typedef struct {
    uint32_t seq;
    uint8_t  level, module, argc;
    uint16_t event_id;
    uint32_t uptime;
    int32_t  args[4];
    char     text[LOG_RING_MAX_TEXT + 1];
} shadow_rec_t;

#define SHADOW_MAX 4096

static void test_stress(void)
{
    static uint8_t buf[2048];
    log_ring_t r;
    log_ring_init(&r, buf, sizeof buf);

    static shadow_rec_t shadow[SHADOW_MAX];   /* ring buffer indexed by seq */
    srand(20260816);

    for (int op = 0; op < 100000; op++) {
        if (rand() % 50 == 0) {
            log_ring_clear(&r);
        } else {
            shadow_rec_t s;
            memset(&s, 0, sizeof s);
            s.level = (uint8_t)(rand() % 6);
            s.module = (uint8_t)(rand() % 9);
            s.event_id = (uint16_t)rand();
            s.uptime = (uint32_t)rand();
            s.argc = (uint8_t)(rand() % 5);
            for (int i = 0; i < s.argc; i++) s.args[i] = rand() - RAND_MAX / 2;
            size_t tl = (size_t)(rand() % (LOG_RING_MAX_TEXT + 1));
            for (size_t i = 0; i < tl; i++) s.text[i] = (char)('A' + rand() % 26);
            s.text[tl] = '\0';
            s.seq = log_ring_push(&r, s.level, s.module, s.event_id, s.uptime,
                                  s.args, s.argc, s.text);
            shadow[s.seq % SHADOW_MAX] = s;
        }

        if (op % 97 == 0) check_invariants(&r);

        /* Full comparison every 1000 ops: every ring record must match
         * the shadow model. */
        if (op % 1000 == 999) {
            log_ring_iter_t it; log_ring_view_t v;
            log_ring_iter_begin(&r, &it);
            while (log_ring_iter_next(&it, &v)) {
                const shadow_rec_t *s = &shadow[v.seq % SHADOW_MAX];
                CHECK(s->seq == v.seq, "shadow seq match");
                CHECK(s->level == v.level && s->module == v.module &&
                      s->event_id == v.event_id && s->uptime == v.uptime_ms,
                      "shadow meta match");
                CHECK(s->argc == v.argc, "shadow argc match");
                for (int i = 0; i < v.argc; i++)
                    CHECK(s->args[i] == v.args[i], "shadow arg match");
                CHECK(strcmp(s->text, v.text) == 0, "shadow text match");
            }
        }
    }
    check_invariants(&r);
}

int main(void)
{
    test_roundtrip();
    printf("1) Roundtrip of all fields ok\n");
    test_truncation();
    printf("2) Truncation + Flag ok\n");
    test_wrap_sweep();
    printf("3) Wrap marker sweep (48 text lengths x 500 pushes) ok\n");
    test_seek();
    printf("4) iter_seek ok\n");
    test_clear();
    printf("5) clear keeps seq ok\n");
    test_stress();
    printf("6) 100k stress against shadow model ok\n");
    printf("== LOG_RING TEST OK (%d checks) ==\n", s_checks);
    return 0;
}
