/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* ============================================================================
 * datapoints.h  —  Generic data-point store for the ESP32 fountain controller.
 * ============================================================================
 *
 * PURPOSE
 *   Implements the "data point" model of the WebSocket protocol spec (v2.1).
 *   Every measurement, status and configuration value is a named, typed data
 *   point. The on-wire catalog (§5.2 of the spec) lives here as ONE list,
 *   dp_list.def. From that single list the C code generates an id enum, the
 *   storage layout, the descriptor table, and the NVS defaults.
 *
 * STORAGE MODEL  ("block device" / PLC process image)
 *   All values are packed into ONE contiguous byte block, g_dp_store.raw
 *   (a uint8_t image). Each data point occupies a fixed slot at a compile-time
 *   offset; the slot SIZE is implied by the data point's type (see dp_type_size
 *   in datapoints.c — "the size lives in the type"). Two ways to touch a value:
 *
 *     1. Generic raw access via (uint8_t* + size), used by the protocol layer:
 *            DP_GET(&out, desc);   DP_SET(desc, &in);     // memcpy by size
 *        This needs no per-data-point code — adding a data point never touches
 *        the read/write path.
 *
 *     2. Typed direct access, used by application modules (fast, 1 instruction):
 *            DP_REF(Fon_Current_Pressure) = 3.18f;
 *            float p = DP_REF(Fon_Current_Pressure);
 *
 *   The descriptor table (g_dp[]) is `const` and therefore lives in flash;
 *   only the value image (~150 bytes) lives in RAM.
 *
 * THREADING (FreeRTOS)
 *   The store is shared between tasks (pump module writes measurements, the
 *   protocol/report task reads them, the protocol task writes config). Rules:
 *     - A single naturally-aligned scalar (<= 4 bytes) read/written via DP_REF
 *       or DP_GET/DP_SET is ATOMIC on the ESP32. So single-writer measurement
 *       updates need NO lock (one owner writes each volatile point).
 *     - Multi-field consistency (e.g. an atomic config write, or reading a
 *       coherent snapshot) IS guarded by an internal mutex. The high-level API
 *       (dp_write_batch, dp_read_into, dp_report_full/changes, dp_config_save)
 *       takes that mutex for you. If you need to read/modify several fields
 *       atomically from app code, wrap them in dp_lock()/dp_unlock().
 *
 * PROTOCOL MAPPING
 *     dp_read   (server->device)  -> dp_read_into()      builds the dp_report body
 *     dp_write  (server->device)  -> dp_write_batch()    atomic; fills errors{}
 *     dp_report (device->server)  -> dp_report_full()    periodic snapshot (§7.8)
 *                                    dp_report_changes()  on-change w/ deadband
 *   The functions return / fill cJSON; the caller adds the message envelope
 *   (v, type, ts, serial, seq, ...). See DOKU/Datapoints.md for a worked example.
 *
 * DEPENDENCIES (ESP-IDF components):  nvs_flash, json (cJSON), esp_system.
 *
 * TO ADD / CHANGE A DATA POINT:  edit dp_list.def (one line). For NVS config
 *   points, also bump DP_CFG_VERSION below and keep NVS points contiguous.
 * ========================================================================== */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>          /* memcpy, memcmp */
#include <stddef.h>          /* offsetof */
#include <math.h>            /* NAN */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"        /* esp_err_t (dp_config_save return type) */
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compile-time configuration ---------------------------------------- */
#define DP_STR_MAX       64        /* reserved bytes for STR data points (WiFi SSID/PW fit) */
/* DP_CFG_VERSION exists ONLY to recognize (and migrate) the legacy blob
 * store one last time. Since v4.26 config lives as one NVS key per point
 * ("d<id>", see dp_list.def id column): layout changes need NO bump and
 * lose NO stored values. Do not change this constant anymore. */
#define DP_CFG_VERSION   7
#define DP_NVS_NAMESPACE "dp"      /* NVS namespace used by this module */

/* ---- Value type system. The size of each type is defined ONCE in -------- *
 * datapoints.c (dp_type_size[]); keep this enum and that table in sync.     */
typedef enum {
    DP_BOOL, DP_U8, DP_U16, DP_U32, DP_U64,
    DP_I8,   DP_I16, DP_I32, DP_F32, DP_ENUM, DP_STR
} dp_type_t;

typedef enum { DP_RO, DP_RW, DP_WO }                 dp_access_t;
typedef enum { DP_VOLATILE, DP_NVS, DP_STATIC }      dp_persist_t;

/* Enum backing the Fon_Current_State data point (spec §5.2). Reported only;
 * settable subset {Off, On, Auto, Manual} is driven via the set_state command. */
typedef enum {
    DP_STATE_INIT = 0, DP_STATE_OFF = 1, DP_STATE_ON = 2,
    DP_STATE_AUTO = 3, DP_STATE_MANUAL = 4, DP_STATE_FAILURE = 5
} dp_state_t;

/* Identity values the application supplies once at startup (see dp_init). */
typedef struct {
    uint64_t    serial;          /* 64-bit serial; encoded as hex on the wire (§4.1) */
    const char *hw_version;      /* SemVer, e.g. "rev-c" */
    const char *sw_version;      /* SemVer, e.g. "2.0.0" */
    uint64_t    build_ms;        /* build timestamp, Unix ms (generated) */
} dp_identity_t;

/* ---- Type tag -> C field declaration (handles the char[] case for STR) -- */
#define DP_FIELD_BOOL(n) uint8_t  n;
#define DP_FIELD_U8(n)   uint8_t  n;
#define DP_FIELD_U16(n)  uint16_t n;
#define DP_FIELD_U32(n)  uint32_t n;
#define DP_FIELD_U64(n)  uint64_t n;
#define DP_FIELD_I8(n)   int8_t   n;
#define DP_FIELD_I16(n)  int16_t  n;
#define DP_FIELD_I32(n)  int32_t  n;
#define DP_FIELD_F32(n)  float    n;
#define DP_FIELD_ENUM(n) uint8_t  n;
#define DP_FIELD_STR(n)  char     n[DP_STR_MAX];
#define DP_FIELD(ty, n)  DP_FIELD_##ty(n)

/* ---- Generated: storage image struct (one field per data point) -------- *
 * Field name is "<name>_img"; offsets come from offsetof() (alignment-safe).*/
typedef struct {
#define DP(nm, ty, ...) DP_FIELD(ty, nm##_img)
#include "dp_list.def"
#undef DP
} dp_image_t;

/* ---- The flat "block device": typed image + raw byte view in one union -- */
typedef union {
    dp_image_t img;
    uint8_t    raw[sizeof(dp_image_t)];
} dp_store_t;

extern dp_store_t g_dp_store;                 /* defined in datapoints.c */
#define DP_BASE     (g_dp_store.raw)          /* uint8_t* base of the block device */
#define DP_REF(nm)  (g_dp_store.img.nm##_img) /* typed lvalue for app code */

/* ---- Generated: id enum (DP_ID_<name>, ... , DP_COUNT) ------------------ */
typedef enum {
#define DP(nm, ...) DP_ID_##nm,
#include "dp_list.def"
#undef DP
    DP_COUNT
} dp_id_t;

/* ---- Descriptor: per data point metadata (lives in flash) -------------- *
 * Note: no storage pointer — the slot is BASE + off; the size is the type. */
typedef struct {
    const char *name;       /* wire identifier (also used for lookup) */
    uint8_t     type;       /* dp_type_t; implies size via dp_type_size[] */
    uint8_t     access;     /* dp_access_t */
    uint8_t     persist;    /* dp_persist_t */
    uint16_t    off;        /* byte offset into DP_BASE */
    uint16_t    nvs_id;     /* stable per-point NVS id ("d<id>"); 0 = none */
    float       min, max;   /* RW validation bounds; NAN = unbounded */
    float       deadband;   /* analog on-change threshold; 0 = none */
} dp_desc_t;

extern const dp_desc_t g_dp[DP_COUNT];        /* descriptor table */
extern const uint8_t   dp_type_size[];        /* dp_type_t -> byte size */

/* ---- Raw value access via (uint8_t* + size) ---------------------------- *
 * Size-switch with constant-length memcpy: each case lowers to a single    *
 * aligned load/store, and stays correct for any alignment/packing.         */
static inline void dp_raw_get(void *dst, const uint8_t *src, uint8_t size)
{
    switch (size) {
        case 1:  *(uint8_t *)dst = *src;     break;
        case 2:  memcpy(dst, src, 2);        break;
        case 4:  memcpy(dst, src, 4);        break;
        case 8:  memcpy(dst, src, 8);        break;
        default: memcpy(dst, src, size);     break;   /* STR */
    }
}
static inline void dp_raw_set(uint8_t *dst, const void *src, uint8_t size)
{
    switch (size) {
        case 1:  *dst = *(const uint8_t *)src; break;
        case 2:  memcpy(dst, src, 2);          break;
        case 4:  memcpy(dst, src, 4);          break;
        case 8:  memcpy(dst, src, 8);          break;
        default: memcpy(dst, src, size);       break;
    }
}

#define DP_SIZE(d)      (dp_type_size[(d)->type])        /* size from the type */
#define DP_SLOT(d)      (DP_BASE + (d)->off)             /* uint8_t* to the slot */
#define DP_GET(dst, d)  dp_raw_get((dst), DP_SLOT(d), DP_SIZE(d))
#define DP_SET(d, src)  dp_raw_set(DP_SLOT(d), (src), DP_SIZE(d))

/* ---- Public API -------------------------------------------------------- */

/* One-time init: create mutex, init NVS, load config (or defaults), set
 * identity. Call once from app_main() before any other dp_* call. */
void dp_init(const dp_identity_t *id);

/* Coarse lock for multi-field consistency from application code. */
bool dp_lock(TickType_t ticks);
void dp_unlock(void);

/* Linear lookup by wire name. Returns NULL if not in the catalog. */
const dp_desc_t *dp_find(const char *name);

/* Sample computed/volatile inputs (heap, temperature, ...) into the image.
 * Call from the report task just before building a report. */
void dp_refresh(void);

/* Encode one data point's value as a cJSON node, reading from `base`
 * (pass DP_BASE for the live store, or a snapshot pointer). Caller owns it. */
cJSON *dp_value_to_json(const dp_desc_t *d, const uint8_t *base);

/* Handle a dp_read: fill `dp_out` (name->value) for the requested `names`
 * (empty/NULL array = full snapshot). Unknown names go into `unknown_out`
 * (may be NULL). Returns the number of values written. */
int dp_read_into(const cJSON *names, cJSON *dp_out, cJSON *unknown_out);

/* Handle a dp_write ATOMICALLY (spec §7.11): validate the whole batch against
 * the resulting state (type, min/max, cross-field constraints); commit ALL or
 * NONE, then persist NVS. Returns true (applied) or false (rejected). On
 * rejection, `errors_out` is filled with name->error_code for each culprit. */
/* Atomic all-or-nothing batch. pbPersisted (optional) reports whether the
 * config region also reached NVS — false means "applied in RAM only"
 * (surfaced to the server as a warning in dp_write_result). */
bool dp_write_batch(const cJSON *dp_in, cJSON *errors_out, bool *pbPersisted);

/* Build a periodic dp_report body: all VOLATILE (measurement/status) points.
 * Resets the on-change baseline. Returns the count. */
int dp_report_full(cJSON *dp_out);
/* Like dp_report_full, but WITHOUT resetting the on-change baseline — for local
 * maintenance sessions (firmware_server.md), so that the cloud deltas stay
 * intact. */
int dp_report_full_noreset(cJSON *dp_out);

/* Build an on-change dp_report body: VOLATILE points that are discrete and
 * changed, or analog and moved past their deadband. Updates the baseline for
 * the reported points. Returns the count (0 => nothing to send). */
int dp_report_changes(cJSON *dp_out);

/* Persist the NVS config region as one blob (called internally by
 * dp_write_batch; exposed for explicit saves). */
esp_err_t dp_config_save(void);

/* Application-specific cross-field validation for a candidate config image
 * (spec §11.2). Implemented in datapoints.c — edit there when constraints
 * change. Returns true if valid; otherwise sets *bad to the offending name. */
bool dp_constraints_ok(const dp_image_t *img, const char **bad);

#ifdef __cplusplus
}
#endif
