/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

/* =============================================================
 * data_modul (data_store) — central data repository.
 *
 * Design pattern: OBSERVER (this module is the Subject).
 * Observers (e.g. fountain_controlling, task_com) register
 * a callback and are notified on changes.
 *
 * Storage: each value is stored compactly as a scaled integer
 * (raw int32 + multiplier/scale) in memory. Macros convert
 * 8-/16-/32-bit raw into the target representation (float32/float64/
 * own type).
 * ============================================================= */

typedef enum {
    DATA_TEMPERATURE = 0,    /* °C   (AM2302)            */
    DATA_HUMIDITY,           /* %rH  (AM2302)            */
    DATA_PRESSURE,           /* bar  (pressure sensor 0-5V) */
    DATA_ESP_INTERNAL_TEMP,  /* °C   (internal sensor)   */
    DATA_RELAY_STATE,        /* 0/1                      */
    DATA_PROTOCOL_VERSION,   /* negotiated version       */
    DATA_TIME_S,             /* Unix time (seconds)      */
    DATA_LOGGING_ALLOW_REMOTE, /* uint8_t: logging config (see debug.h) */
    DATA_KEY_COUNT
} data_key_t;

/* For observers: listen to every change. */
#define DATA_KEY_ANY ((data_key_t)(DATA_KEY_COUNT))

/* Scaling macros: raw <-> real value (macro handles the conversion). */
#define DATA_RAW_TO_FLOAT(raw, scale)  ((float)(raw) * (float)(scale))
#define DATA_FLOAT_TO_RAW(val, scale)  \
    ((int32_t)((val) / (float)(scale) + ((val) >= 0.0f ? 0.5f : -0.5f)))

typedef void (*data_observer_cb_t)(data_key_t eKey, void *pvCtx);

bool data_store_init(void);

/* Float access (uses the stored scale factor). */
bool data_store_float_set(data_key_t eKey, float flValue);
bool data_store_float_get(data_key_t eKey, float *pflOut);

/* Integer access (scale = 1) for states/version/time. */
bool data_store_u32_set(data_key_t eKey, uint32_t ulValue);
bool data_store_u32_get(data_key_t eKey, uint32_t *pulOut);

/* OBSERVER: register a callback. eKey == DATA_KEY_ANY -> all keys. */
bool data_store_observer_subscribe(data_key_t eKey, data_observer_cb_t pfnCb, void *pvCtx);
