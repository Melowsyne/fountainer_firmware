/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* ============================================================================
 * datapoints.c  —  Implementation of the generic data-point store.
 * See datapoints.h for the architecture overview and DOKU/Datapoints.md for an
 * integration walkthrough. All comments are intentionally explicit so the
 * module can be picked up and re-understood quickly later.
 * ========================================================================== */
#include "datapoints.h"

#include <stdio.h>          /* snprintf */
#include <stdlib.h>         /* strtoull */

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"     /* esp_get_free_heap_size / _minimum_ */
#include "esp_heap_caps.h"  /* heap_caps_get_largest_free_block */
#include "esp_log.h"

static const char *TAG = "dp";

/* ---------------------------------------------------------------------------
 * 1. The size that "lives in the type". Index order MUST match dp_type_t.
 * ------------------------------------------------------------------------- */
const uint8_t dp_type_size[] = {
    [DP_BOOL] = 1, [DP_U8] = 1, [DP_U16] = 2, [DP_U32] = 4, [DP_U64] = 8,
    [DP_I8]   = 1, [DP_I16] = 2, [DP_I32] = 4, [DP_F32] = 4, [DP_ENUM] = 1,
    [DP_STR]  = DP_STR_MAX,
};

/* ---------------------------------------------------------------------------
 * 2. The block device (live image) and the generated descriptor table.
 *    g_dp_store is the single RAM image; g_dp[] is the const flash catalog.
 * ------------------------------------------------------------------------- */
dp_store_t g_dp_store;

const dp_desc_t g_dp[DP_COUNT] = {
#define DP(nm, ty, ac, pe, id, dv, mn, mx, db)                                \
    [DP_ID_##nm] = { .name = #nm, .type = DP_##ty, .access = DP_##ac,         \
                     .persist = DP_##pe, .off = offsetof(dp_image_t, nm##_img),\
                     .nvs_id = (id),                                          \
                     .min = (mn), .max = (mx), .deadband = (db) },
#include "dp_list.def"
#undef DP
};

/* Baseline image used for on-change detection (last values reported). */
static dp_image_t s_last;

/* FreeRTOS mutex guarding multi-field consistency (see header "THREADING"). */
static SemaphoreHandle_t s_mutex;

/* Contiguous NVS config region within the image, computed once at init. */
static uint16_t s_cfg_off, s_cfg_size;

/* ---------------------------------------------------------------------------
 * 3. Lock helpers.
 * ------------------------------------------------------------------------- */
bool dp_lock(TickType_t ticks)
{
    return s_mutex && xSemaphoreTake(s_mutex, ticks) == pdTRUE;
}
void dp_unlock(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

/* ---------------------------------------------------------------------------
 * 4. Lookup and NVS-region computation.
 * ------------------------------------------------------------------------- */
const dp_desc_t *dp_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < DP_COUNT; i++)
        if (strcmp(g_dp[i].name, name) == 0) return &g_dp[i];
    return NULL;                         /* -> "unknown_name" */
}

/* Derive [s_cfg_off, s_cfg_off + s_cfg_size) from the NVS descriptors.
 * Assumes NVS points are contiguous in dp_list.def (they are, by grouping). */
static void dp_compute_cfg_region(void)
{
    uint32_t lo = UINT32_MAX, hi = 0;
    for (size_t i = 0; i < DP_COUNT; i++) {
        if (g_dp[i].persist != DP_NVS) continue;
        uint32_t a = g_dp[i].off;
        uint32_t b = a + dp_type_size[g_dp[i].type];
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    if (lo == UINT32_MAX) { s_cfg_off = 0; s_cfg_size = 0; return; }
    s_cfg_off  = (uint16_t)lo;
    s_cfg_size = (uint16_t)(hi - lo);
}

/* ---------------------------------------------------------------------------
 * 5. JSON codec — the ONLY place that needs to know the concrete type, in
 *    order to format / parse the value. Raw byte movement stays generic.
 * ------------------------------------------------------------------------- */
cJSON *dp_value_to_json(const dp_desc_t *d, const uint8_t *base)
{
    const uint8_t *p = base + d->off;
    switch (d->type) {
        case DP_BOOL: return cJSON_CreateBool(*p);
        case DP_U8:   return cJSON_CreateNumber(*p);
        case DP_ENUM: return cJSON_CreateNumber(*p);
        case DP_I8:   return cJSON_CreateNumber(*(const int8_t *)p);
        case DP_U16:  { uint16_t v; memcpy(&v, p, 2); return cJSON_CreateNumber(v); }
        case DP_I16:  { int16_t  v; memcpy(&v, p, 2); return cJSON_CreateNumber(v); }
        case DP_U32:  { uint32_t v; memcpy(&v, p, 4); return cJSON_CreateNumber((double)v); }
        case DP_I32:  { int32_t  v; memcpy(&v, p, 4); return cJSON_CreateNumber(v); }
        case DP_F32:  { float    v; memcpy(&v, p, 4); return cJSON_CreateNumber(v); }
        case DP_U64:  {            /* 64-bit -> hex string (spec §2/§4.1) */
            uint64_t v; memcpy(&v, p, 8);
            char s[17]; snprintf(s, sizeof s, "%016llX", (unsigned long long)v);
            return cJSON_CreateString(s);
        }
        case DP_STR:  return cJSON_CreateString((const char *)p);  /* NUL-terminated */
    }
    return NULL;
}

/* Parse one JSON value into raw bytes `dst` (a slot in a tentative image),
 * applying type and range checks. Returns true on success; on failure sets
 * *err to an error code from the spec (type_mismatch / out_of_range). */
static bool dp_value_from_json(const dp_desc_t *d, const cJSON *v,
                               uint8_t *dst, const char **err)
{
    /* Numeric types share the range check. */
    if (d->type != DP_BOOL && d->type != DP_U64 && d->type != DP_STR) {
        if (!cJSON_IsNumber(v)) { *err = "type_mismatch"; return false; }
        double num = v->valuedouble;
        if (!isnan(d->min) && num < d->min) { *err = "out_of_range"; return false; }
        if (!isnan(d->max) && num > d->max) { *err = "out_of_range"; return false; }
        switch (d->type) {
            case DP_U8: case DP_ENUM: { uint8_t  x = (uint8_t)num;  memcpy(dst, &x, 1); break; }
            case DP_I8:               { int8_t   x = (int8_t)num;   memcpy(dst, &x, 1); break; }
            case DP_U16:              { uint16_t x = (uint16_t)num; memcpy(dst, &x, 2); break; }
            case DP_I16:              { int16_t  x = (int16_t)num;  memcpy(dst, &x, 2); break; }
            case DP_U32:              { uint32_t x = (uint32_t)num; memcpy(dst, &x, 4); break; }
            case DP_I32:              { int32_t  x = (int32_t)num;  memcpy(dst, &x, 4); break; }
            case DP_F32:              { float    x = (float)num;    memcpy(dst, &x, 4); break; }
            default: *err = "type_mismatch"; return false;
        }
        return true;
    }
    switch (d->type) {
        case DP_BOOL:
            if (!cJSON_IsBool(v)) { *err = "type_mismatch"; return false; }
            { uint8_t b = cJSON_IsTrue(v) ? 1 : 0; memcpy(dst, &b, 1); }
            return true;
        case DP_U64:   /* accepted as decimal or hex string */
            if (!cJSON_IsString(v)) { *err = "type_mismatch"; return false; }
            { char *end = NULL;
              uint64_t x = strtoull(v->valuestring, &end, 0);
              /* No digits consumed or garbage trailing -> reject instead of
               * silently writing "0" ("hello" -> error). */
              if (end == v->valuestring || (end && *end != '\0')) {
                  *err = "type_mismatch"; return false;
              }
              memcpy(dst, &x, 8); }
            return true;
        case DP_STR:
            if (!cJSON_IsString(v)) { *err = "type_mismatch"; return false; }
            /* REJECT overlong strings instead of silently truncating to
             * DP_STR_MAX-1 (otherwise the device would acknowledge "applied"
             * for a truncated value). */
            if (strlen(v->valuestring) >= DP_STR_MAX) { *err = "too_long"; return false; }
            strncpy((char *)dst, v->valuestring, DP_STR_MAX - 1);
            ((char *)dst)[DP_STR_MAX - 1] = '\0';
            return true;
        default:
            *err = "type_mismatch";
            return false;
    }
}

/* ---------------------------------------------------------------------------
 * 6. dp_read — fill an object with requested (or all) values.
 * ------------------------------------------------------------------------- */
int dp_read_into(const cJSON *names, cJSON *dp_out, cJSON *unknown_out)
{
    dp_image_t snap;
    dp_lock(portMAX_DELAY);
    memcpy(&snap, &g_dp_store.img, sizeof snap);
    dp_unlock();

    int n = 0;
    if (names == NULL || cJSON_GetArraySize(names) == 0) {     /* full snapshot */
        for (size_t i = 0; i < DP_COUNT; i++) {
            cJSON_AddItemToObject(dp_out, g_dp[i].name,
                                  dp_value_to_json(&g_dp[i], (const uint8_t *)&snap));
            n++;
        }
        return n;
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, names) {
        if (!cJSON_IsString(e)) continue;
        const dp_desc_t *d = dp_find(e->valuestring);
        if (d) {
            cJSON_AddItemToObject(dp_out, d->name,
                                  dp_value_to_json(d, (const uint8_t *)&snap));
            n++;
        } else if (unknown_out) {
            cJSON_AddItemToArray(unknown_out, cJSON_CreateString(e->valuestring));
        }
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * 7. dp_write — ATOMIC batch (spec §7.11): all-or-nothing.
 * ------------------------------------------------------------------------- */
bool dp_write_batch(const cJSON *dp_in, cJSON *errors_out, bool *pbPersisted)
{
    if (pbPersisted) *pbPersisted = true;
    /* Work on a private copy so a rejected batch leaves the store untouched. */
    dp_image_t tmp;
    bool ok = true;
    const cJSON *item;

    /* The ENTIRE transaction runs under ONE lock hold: snapshot -> validation
     * -> cross-field constraints -> commit. If the mutex were released between
     * snapshot and commit (as in an earlier version), two concurrent writers
     * (cloud + local, or two local sessions) could each validate against a
     * stale snapshot and JOINTLY violate the constraints even though each
     * batch is valid on its own (TOCTOU). Only CPU work runs under the lock
     * (cJSON/strcmp/memcpy) — NO NVS, NO HAL. */
    dp_lock(portMAX_DELAY);
    memcpy(&tmp, &g_dp_store.img, sizeof tmp);

    cJSON_ArrayForEach(item, dp_in) {
        const char *key = item->string ? item->string : "?";
        const char *err = NULL;
        const dp_desc_t *d = dp_find(key);
        if (!d)                       err = "unknown_name";
        else if (d->access != DP_RW)  err = "read_only";
        else                          dp_value_from_json(d, item,
                                          ((uint8_t *)&tmp) + d->off, &err);
        if (err) { ok = false; if (errors_out) cJSON_AddStringToObject(errors_out, key, err); }
    }

    if (ok) {                         /* cross-field constraints over the result */
        const char *bad = NULL;
        if (!dp_constraints_ok(&tmp, &bad)) {
            ok = false;
            if (errors_out) cJSON_AddStringToObject(errors_out,
                                bad ? bad : "<constraint>", "constraint_violation");
        }
    }
    if (!ok) { dp_unlock(); return false; }   /* rejected: nothing committed */

    /* Commit: copy back EXACTLY the written fields (NVS config and volatile
     * command/label points alike) — never whole regions, so concurrent
     * volatile measurement updates survive and the list layout stays free
     * (no contiguity requirement since the per-key store, v4.26). */
    cJSON_ArrayForEach(item, dp_in) {
        const dp_desc_t *d = dp_find(item->string ? item->string : "?");
        if (d && d->access == DP_RW)
            memcpy(DP_BASE + d->off, ((const uint8_t *)&tmp) + d->off,
                   dp_type_size[d->type]);
    }
    dp_unlock();

    /* NVS commit only AFTER the unlock (a page erase can take >100 ms and must
     * not block the 200 ms pump control loop via the dp lock). */
    if (dp_config_save() != ESP_OK) {
        ESP_LOGW(TAG, "config applied in RAM but NVS save failed");
        if (pbPersisted) *pbPersisted = false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * 8. dp_report — periodic (full) and on-change (delta) bodies.
 * ------------------------------------------------------------------------- */
int dp_report_full(cJSON *dp_out)
{
    dp_image_t snap;
    dp_lock(portMAX_DELAY);
    memcpy(&snap, &g_dp_store.img, sizeof snap);
    dp_unlock();

    int n = 0;
    for (size_t i = 0; i < DP_COUNT; i++) {
        if (g_dp[i].persist != DP_VOLATILE) continue;   /* measurements/status only */
        cJSON_AddItemToObject(dp_out, g_dp[i].name,
                              dp_value_to_json(&g_dp[i], (const uint8_t *)&snap));
        n++;
    }
    memcpy(&s_last, &snap, sizeof s_last);              /* reset on-change baseline */
    return n;
}

/* Full snapshot WITHOUT baseline reset: for ADDITIONAL consumers (local
 * maintenance sessions, firmware_server.md) — the on-change deltas of the
 * cloud session must not be "stolen" by local polling. */
int dp_report_full_noreset(cJSON *dp_out)
{
    dp_image_t snap;
    dp_lock(portMAX_DELAY);
    memcpy(&snap, &g_dp_store.img, sizeof snap);
    dp_unlock();

    int n = 0;
    for (size_t i = 0; i < DP_COUNT; i++) {
        if (g_dp[i].persist != DP_VOLATILE) continue;
        cJSON_AddItemToObject(dp_out, g_dp[i].name,
                              dp_value_to_json(&g_dp[i], (const uint8_t *)&snap));
        n++;
    }
    return n;
}

int dp_report_changes(cJSON *dp_out)
{
    dp_image_t snap;
    dp_lock(portMAX_DELAY);
    memcpy(&snap, &g_dp_store.img, sizeof snap);
    dp_unlock();

    int n = 0;
    for (size_t i = 0; i < DP_COUNT; i++) {
        const dp_desc_t *d = &g_dp[i];
        if (d->persist != DP_VOLATILE) continue;

        /* Eligible for on-change: discrete (bool/enum), or analog with a
         * deadband. Pure analog without a deadband is periodic-only. */
        bool eligible = (d->type == DP_BOOL || d->type == DP_ENUM) || (d->deadband > 0.0f);
        if (!eligible) continue;

        const uint8_t *cur  = (const uint8_t *)&snap + d->off;
        uint8_t       *last = (uint8_t *)&s_last + d->off;
        uint8_t        sz   = dp_type_size[d->type];
        bool changed;
        if (d->deadband > 0.0f && d->type == DP_F32) {
            float c, l; memcpy(&c, cur, 4); memcpy(&l, last, 4);
            changed = fabsf(c - l) >= d->deadband;
        } else {
            changed = memcmp(cur, last, sz) != 0;
        }
        if (changed) {
            cJSON_AddItemToObject(dp_out, d->name,
                                  dp_value_to_json(d, (const uint8_t *)&snap));
            memcpy(last, cur, sz);                      /* advance baseline */
            n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * 9. Cross-field constraints (spec §11.2). Edit here when rules change.
 * ------------------------------------------------------------------------- */
bool dp_constraints_ok(const dp_image_t *m, const char **bad)
{
    if (!(m->Fon_Min_Pressure_img < m->Fon_Max_Pressure_img))        { *bad = "Fon_Max_Pressure";        return false; }
    if (!(m->Fon_Max_Pressure_img < m->Fon_Alert_High_Pressure_img)) { *bad = "Fon_Alert_High_Pressure"; return false; }
    if (!(m->Fon_Alert_Low_Pressure_img < m->Fon_Min_Pressure_img))  { *bad = "Fon_Alert_Low_Pressure";  return false; }
    if (m->Fon_Max_On_Time_img < 10)                                 { *bad = "Fon_Max_On_Time";         return false; }
    if (!(m->Fon_Min_On_Time_img < m->Fon_Max_On_Time_img))          { *bad = "Fon_Min_On_Time";         return false; }
    if (!(m->Fon_Dry_Run_Detect_Time_img < m->Fon_Max_On_Time_img))  { *bad = "Fon_Dry_Run_Detect_Time"; return false; }
    if (m->Fon_Report_Interval_img < 1 || m->Fon_Report_Interval_img > 3600)
                                                                     { *bad = "Fon_Report_Interval";     return false; }
    return true;
}

/* ---------------------------------------------------------------------------
 * 10. NVS persistence — ONE KEY PER POINT ("d<id>", fixed-size blob of the
 *     type's byte size). Stage 2 of the datapoints v2 plan: stored values
 *     survive ANY dp_list.def change; a type-size mismatch or an
 *     out-of-range value simply falls back to the compiled default. NVS
 *     itself deduplicates identical blob writes, so a full save is cheap.
 * ------------------------------------------------------------------------- */
static void dp_nvs_key(char *out, const dp_desc_t *d)   /* "d101" ... */
{
    snprintf(out, 8, "d%u", (unsigned)d->nvs_id);
}

esp_err_t dp_config_save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(DP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    dp_image_t snap;
    dp_lock(portMAX_DELAY);
    memcpy(&snap, &g_dp_store.img, sizeof snap);
    dp_unlock();

    char key[8];
    for (size_t i = 0; i < DP_COUNT && e == ESP_OK; i++) {
        const dp_desc_t *d = &g_dp[i];
        if (d->persist != DP_NVS || d->nvs_id == 0) continue;
        dp_nvs_key(key, d);
        e = nvs_set_blob(h, key, (const uint8_t *)&snap + d->off,
                         dp_type_size[d->type]);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* Numeric slot value as double (for the load-time range validation). */
static bool dp_slot_as_double(const dp_desc_t *d, const uint8_t *p, double *out)
{
    switch (d->type) {
        case DP_BOOL: case DP_U8: case DP_ENUM: *out = *p; return true;
        case DP_I8:  *out = *(const int8_t *)p; return true;
        case DP_U16: { uint16_t v; memcpy(&v, p, 2); *out = v; } return true;
        case DP_I16: { int16_t  v; memcpy(&v, p, 2); *out = v; } return true;
        case DP_U32: { uint32_t v; memcpy(&v, p, 4); *out = v; } return true;
        case DP_I32: { int32_t  v; memcpy(&v, p, 4); *out = v; } return true;
        case DP_F32: { float    v; memcpy(&v, p, 4); *out = v; } return true;
        default: return false;      /* U64/STR: no range semantics */
    }
}

/* Load every persisted point by its key. Missing key -> keep the default;
 * size mismatch (type changed) or min/max violation -> keep the default. */
static void dp_config_load(void)
{
    nvs_handle_t h;
    if (nvs_open(DP_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no NVS config yet; using defaults");
        return;
    }
    char key[8];
    uint8_t buf[DP_STR_MAX];
    int loaded = 0, defaults = 0;
    for (size_t i = 0; i < DP_COUNT; i++) {
        const dp_desc_t *d = &g_dp[i];
        if (d->persist != DP_NVS || d->nvs_id == 0) continue;
        dp_nvs_key(key, d);
        size_t len = sizeof buf;
        if (nvs_get_blob(h, key, buf, &len) != ESP_OK) { defaults++; continue; }
        if (len != dp_type_size[d->type]) {              /* type changed */
            ESP_LOGW(TAG, "%s: stored size %u != %u — default kept",
                     d->name, (unsigned)len, (unsigned)dp_type_size[d->type]);
            defaults++;
            continue;
        }
        double num;
        if (dp_slot_as_double(d, buf, &num) &&
            ((!isnan(d->min) && num < d->min) ||
             (!isnan(d->max) && num > d->max))) {        /* range shrank */
            ESP_LOGW(TAG, "%s: stored value out of range — default kept",
                     d->name);
            defaults++;
            continue;
        }
        if (d->type == DP_STR) buf[DP_STR_MAX - 1] = '\0';
        dp_lock(portMAX_DELAY);
        memcpy(DP_BASE + d->off, buf, len);
        dp_unlock();
        loaded++;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "config loaded per key: %d values, %d defaults",
             loaded, defaults);
}

/* One-time migration from the legacy single-blob store: if the old
 * "cfg"/"ver" pair matches this build's layout, its content is adopted,
 * re-saved per key and the blob erased. Runs BEFORE dp_config_load. */
static void dp_config_migrate_blob(void)
{
    nvs_handle_t h;
    if (nvs_open(DP_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    uint16_t ver = 0;
    size_t   len = 0;
    bool have = nvs_get_u16(h, "ver", &ver) == ESP_OK &&
                nvs_get_blob(h, "cfg", NULL, &len) == ESP_OK;
    if (have && ver == DP_CFG_VERSION && len == s_cfg_size) {
        dp_lock(portMAX_DELAY);
        nvs_get_blob(h, "cfg", DP_BASE + s_cfg_off, &len);
        dp_unlock();
        nvs_close(h);
        ESP_LOGI(TAG, "legacy blob (ver=%u, %u bytes) migrated to per-key store",
                 ver, (unsigned)len);
        dp_config_save();                       /* write the per-key set   */
        if (nvs_open(DP_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, "cfg");
            nvs_erase_key(h, "ver");
            nvs_commit(h);
            nvs_close(h);
        }
        return;
    }
    if (have) {                                  /* stale/incompatible blob */
        ESP_LOGW(TAG, "legacy blob incompatible (ver=%u) — discarded", ver);
        nvs_erase_key(h, "cfg");
        nvs_erase_key(h, "ver");
        nvs_commit(h);
    }
    nvs_close(h);
}

/* Write the compiled-in defaults into the image. Generated from dp_list.def:
 * only NVS points get a default; VOLATILE/STATIC expand to nothing. */
static void dp_set_defaults(void)
{
    /* Scalar NVS points get their compiled-in default assigned. STR NVS points
     * keep the empty (zeroed) default from the initial memset — a char[] array
     * cannot be assigned to, so its DP_ASSIGN is a no-op. */
#define DP_ASSIGN_BOOL(nm, dv) DP_REF(nm) = (dv);
#define DP_ASSIGN_U8(nm, dv)   DP_REF(nm) = (dv);
#define DP_ASSIGN_U16(nm, dv)  DP_REF(nm) = (dv);
#define DP_ASSIGN_U32(nm, dv)  DP_REF(nm) = (dv);
#define DP_ASSIGN_U64(nm, dv)  DP_REF(nm) = (dv);
#define DP_ASSIGN_I8(nm, dv)   DP_REF(nm) = (dv);
#define DP_ASSIGN_I16(nm, dv)  DP_REF(nm) = (dv);
#define DP_ASSIGN_I32(nm, dv)  DP_REF(nm) = (dv);
#define DP_ASSIGN_F32(nm, dv)  DP_REF(nm) = (dv);
#define DP_ASSIGN_ENUM(nm, dv) DP_REF(nm) = (dv);
#define DP_ASSIGN_STR(nm, dv)  /* empty default from memset (no array assignment) */
#define DP_DEFAULT_NVS(nm, ty, dv)      DP_ASSIGN_##ty(nm, dv)
#define DP_DEFAULT_VOLATILE(nm, ty, dv) /* nothing */
#define DP_DEFAULT_STATIC(nm, ty, dv)   /* set from identity in dp_init */
#define DP(nm, ty, ac, pe, id, dv, mn, mx, db) DP_DEFAULT_##pe(nm, ty, dv)
#include "dp_list.def"
#undef DP
#undef DP_DEFAULT_NVS
#undef DP_DEFAULT_VOLATILE
#undef DP_DEFAULT_STATIC
#undef DP_ASSIGN_BOOL
#undef DP_ASSIGN_U8
#undef DP_ASSIGN_U16
#undef DP_ASSIGN_U32
#undef DP_ASSIGN_U64
#undef DP_ASSIGN_I8
#undef DP_ASSIGN_I16
#undef DP_ASSIGN_I32
#undef DP_ASSIGN_F32
#undef DP_ASSIGN_ENUM
#undef DP_ASSIGN_STR
}

/* ---------------------------------------------------------------------------
 * 11. Refresh of computed inputs (process-image style). Wire to real sources.
 * ------------------------------------------------------------------------- */
void dp_refresh(void)
{
    DP_REF(System_Memory_Free)        = (uint32_t)esp_get_free_heap_size();
    DP_REF(System_Min_Memory_Free)    = (uint32_t)esp_get_minimum_free_heap_size();
    /* Largest contiguous block — fragmentation/leak metric for stress
     * observation (readable live via dp_read). */
    DP_REF(System_Largest_Free_Block) =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    /* TODO: wire these to your board/SDK sources (left as placeholders):
     *   DP_REF(System_Temperature) = read_internal_temp();  // temperature_sensor driver
     *   DP_REF(System_Flash_Free)  = read_flash_free();      // esp_partition / app logic
     *   DP_REF(System_RSSI)        = read_wifi_rssi();        // esp_wifi_sta_get_ap_info()
     *   DP_REF(System_Utilization) = read_cpu_load();         // FreeRTOS runtime stats
     */
}

/* ---------------------------------------------------------------------------
 * 12. Init — call once from app_main() before any other dp_* function.
 * ------------------------------------------------------------------------- */
void dp_init(const dp_identity_t *id)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    /* NVS must be initialized once per app; safe to call here. */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    memset(&g_dp_store, 0, sizeof g_dp_store);
    dp_compute_cfg_region();       /* only the blob MIGRATION still needs it */
    dp_set_defaults();             /* defaults first ... */
    dp_config_migrate_blob();      /* one-time legacy-blob adoption ...      */
    dp_config_load();              /* ... then per-key values where present  */

    /* Static identity values (spec §4.1 / §5.2). */
    if (id) {
        DP_REF(Device_Serial_Number) = id->serial;
        if (id->hw_version) { strncpy(DP_REF(Device_HW_Version), id->hw_version, DP_STR_MAX - 1);
                              DP_REF(Device_HW_Version)[DP_STR_MAX - 1] = '\0'; }
        if (id->sw_version) { strncpy(DP_REF(Device_SW_Version), id->sw_version, DP_STR_MAX - 1);
                              DP_REF(Device_SW_Version)[DP_STR_MAX - 1] = '\0'; }
        DP_REF(Device_Build_Version) = id->build_ms;
    }

    memcpy(&s_last, &g_dp_store.img, sizeof s_last);   /* seed on-change baseline */
    ESP_LOGI(TAG, "initialized: %d data points, cfg region [%u..%u] (%u bytes)",
             (int)DP_COUNT, s_cfg_off, (unsigned)(s_cfg_off + s_cfg_size), s_cfg_size);
}
