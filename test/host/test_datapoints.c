/* Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * Host test for datapoints.c (dp_write_batch / dp_value_from_json) — without
 * ESP-IDF, against lightweight mocks (test/host/mocks/, DP mutex = real pthread).
 * Covers:
 *   A) Validation: unknown_name / read_only / out_of_range / type_mismatch /
 *      too_long (F2) / constraint_violation.
 *   B) Atomic all-or-nothing batch: one error -> nothing is committed.
 *   C) Concurrency stress test (F1): two threads write overlapping values to
 *      cross-field-constrained DPs while a checker thread continuously reads
 *      snapshots — the constraints must NEVER be violated. (Before the
 *      TOCTOU fix this would have been possible.)
 */
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "datapoints.h"

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); s_fail = 1; } \
    else         { printf("  ok  : %s\n", (msg)); } } while (0)

/* One dp_write with exactly one key -> determine status/error. The error text
 * is COPIED into a static buffer (the cJSON is freed afterwards). */
static char s_err[32];
static bool write_one(const char *name, cJSON *value, const char **err_out)
{
    s_err[0] = '\0';
    cJSON *dp = cJSON_CreateObject();
    cJSON_AddItemToObject(dp, name, value);   /* takes ownership */
    cJSON *errs = cJSON_CreateObject();
    bool ok = dp_write_batch(dp, errs, NULL);
    const cJSON *e = cJSON_GetObjectItem(errs, name);
    if (e && cJSON_IsString(e)) {
        strncpy(s_err, e->valuestring, sizeof s_err - 1);
        s_err[sizeof s_err - 1] = '\0';
    }
    cJSON_Delete(dp);
    cJSON_Delete(errs);
    if (err_out) *err_out = s_err[0] ? s_err : NULL;
    return ok;
}

static double read_num(const char *name)
{
    cJSON *names = cJSON_CreateArray();
    cJSON_AddItemToArray(names, cJSON_CreateString(name));
    cJSON *out = cJSON_CreateObject();
    dp_read_into(names, out, NULL);
    const cJSON *v = cJSON_GetObjectItem(out, name);
    double r = (v && cJSON_IsNumber(v)) ? v->valuedouble : NAN;
    cJSON_Delete(names);
    cJSON_Delete(out);
    return r;
}

static void test_validation(void)
{
    const char *err;
    printf("A) Validation:\n");

    CHECK(!write_one("Does_Not_Exist", cJSON_CreateNumber(1), &err) &&
          err && strcmp(err, "unknown_name") == 0, "unknown_name");

    CHECK(!write_one("Fon_Current_State", cJSON_CreateNumber(1), &err) &&
          err && strcmp(err, "read_only") == 0, "read_only (RO point)");

    CHECK(!write_one("Fon_Report_Interval", cJSON_CreateNumber(5000), &err) &&
          err && strcmp(err, "out_of_range") == 0, "out_of_range (>max)");

    CHECK(!write_one("Fon_Report_Interval", cJSON_CreateString("hello"), &err) &&
          err && strcmp(err, "type_mismatch") == 0, "type_mismatch (str for num)");

    char big[128];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    CHECK(!write_one("Network_SSID", cJSON_CreateString(big), &err) &&
          err && strcmp(err, "too_long") == 0, "too_long (STR > DP_STR_MAX)");

    /* Fon_Min_Pressure default 2.0, Max 3.5 -> Min=5.0 violates Min<Max.
     * The error is reported under the constraint field (Fon_Max_Pressure);
     * what matters here: rejected + store unchanged (Min stays 2.0). */
    double min_before = read_num("Fon_Min_Pressure");
    CHECK(!write_one("Fon_Min_Pressure", cJSON_CreateNumber(5.0), &err),
          "constraint_violation (Min>=Max) rejected");
    CHECK(read_num("Fon_Min_Pressure") == min_before,
          "constraint-violating value NOT committed");
}

static void test_atomic(void)
{
    printf("B) Atomic batch:\n");
    double before = read_num("Fon_Report_Interval");

    /* valid + invalid in ONE batch -> everything discarded. */
    cJSON *dp = cJSON_CreateObject();
    cJSON_AddNumberToObject(dp, "Fon_Report_Interval", 42);   /* valid */
    cJSON_AddNumberToObject(dp, "Fon_Max_On_Time", 1);        /* < 10 -> constraint */
    bool ok = dp_write_batch(dp, NULL, NULL);
    cJSON_Delete(dp);

    double after = read_num("Fon_Report_Interval");
    CHECK(!ok, "mixed batch is rejected");
    CHECK(before == after, "valid key NOT committed (all-or-nothing)");

    /* a purely valid batch is accepted. */
    cJSON *ok_dp = cJSON_CreateObject();
    cJSON_AddNumberToObject(ok_dp, "Fon_Report_Interval", 33);
    bool ok2 = dp_write_batch(ok_dp, NULL, NULL);
    cJSON_Delete(ok_dp);
    CHECK(ok2 && read_num("Fon_Report_Interval") == 33, "valid batch committed");
}

/* ----- C) Concurrency stress test --------------------------------------- */
#define STRESS_ITERS 20000
static atomic_int s_stop;

static void seed_pressures(void)
{
    cJSON *dp = cJSON_CreateObject();
    cJSON_AddNumberToObject(dp, "Fon_Alert_Low_Pressure", 0.2);
    cJSON_AddNumberToObject(dp, "Fon_Min_Pressure", 1.0);
    cJSON_AddNumberToObject(dp, "Fon_Max_Pressure", 8.0);
    cJSON_AddNumberToObject(dp, "Fon_Alert_High_Pressure", 9.5);
    bool ok = dp_write_batch(dp, NULL, NULL);
    cJSON_Delete(dp);
    assert(ok);
}

/* Continuously writes ONLY Min or ONLY Max with overlapping values —
 * exactly the pattern that could produce Min>=Max without the TOCTOU fix. */
static void *writer(void *arg)
{
    const char *name = (const char *)arg;
    unsigned seed = (unsigned)(size_t)arg;
    for (int i = 0; i < STRESS_ITERS; i++) {
        double v = 1.5 + (rand_r(&seed) % 6000) / 1000.0;   /* 1.5 .. 7.5 */
        write_one(name, cJSON_CreateNumber(v), NULL);        /* may reject */
    }
    return NULL;
}

/* Continuously reads snapshots and checks the invariant Min < Max. */
static void *checker(void *arg)
{
    (void)arg;
    long violations = 0, reads = 0;
    while (!atomic_load(&s_stop)) {
        cJSON *names = cJSON_CreateArray();
        cJSON_AddItemToArray(names, cJSON_CreateString("Fon_Min_Pressure"));
        cJSON_AddItemToArray(names, cJSON_CreateString("Fon_Max_Pressure"));
        cJSON *out = cJSON_CreateObject();
        dp_read_into(names, out, NULL);
        double mn = cJSON_GetObjectItem(out, "Fon_Min_Pressure")->valuedouble;
        double mx = cJSON_GetObjectItem(out, "Fon_Max_Pressure")->valuedouble;
        if (!(mn < mx)) violations++;
        reads++;
        cJSON_Delete(names);
        cJSON_Delete(out);
    }
    printf("  (checker: %ld snapshots, %ld violations)\n", reads, violations);
    return (void *)(size_t)violations;
}

static void test_concurrency(void)
{
    printf("C) Concurrency stress (F1):\n");
    seed_pressures();
    atomic_store(&s_stop, 0);

    pthread_t wa, wb, ck;
    pthread_create(&ck, NULL, checker, NULL);
    pthread_create(&wa, NULL, writer, (void *)"Fon_Min_Pressure");
    pthread_create(&wb, NULL, writer, (void *)"Fon_Max_Pressure");
    pthread_join(wa, NULL);
    pthread_join(wb, NULL);
    atomic_store(&s_stop, 1);
    void *v = NULL;
    pthread_join(ck, &v);

    CHECK((size_t)v == 0, "no constraint violation under concurrency");
}

int main(void)
{
    dp_init(NULL);
    printf("== test_datapoints ==\n");
    test_validation();
    test_atomic();
    test_concurrency();
    printf(s_fail ? "== FAIL ==\n" : "== PASS ==\n");
    return s_fail;
}
