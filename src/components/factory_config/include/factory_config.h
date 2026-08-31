/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * factory_config — per-device production data (series production).
 *
 * Reads the separate NVS partition "factory" (namespace "device"), which is
 * written by the production station (tools/gen_factory_nvs.py ->
 * factory.bin @0xF40000). The rest of the code NEVER accesses the partition
 * directly, only this interface — so the storage stays replaceable
 * (later e.g. encrypted NVS, eFuse, DS peripheral).
 *
 * If the partition is missing or empty/incomplete, the compiled-in fallback
 * identity applies (bit-exact the previous testbed identity) — so the same
 * firmware image runs unchanged on the existing device without a factory
 * partition (OTA compatibility, ota_0/ota_1 untouched).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define FACTORY_PART_LABEL "factory"
#define FACTORY_NAMESPACE  "device"

#define FACTORY_STR_MAX    64
#define FACTORY_HOST_MAX   128
#define FACTORY_BEARER_MAX 96
#define FACTORY_KID_MAX    16
#define FACTORY_BATCH_MAX  32
#define FACTORY_DATE_MAX   16
#define FACTORY_PEM_MAX    3072   /* RSA-3072: cert ~1.9 KB, key ~2.5 KB; NVS string limit 4000 */

typedef struct {
    bool     from_factory;                  /* false -> compiled-in fallback identity      */
    uint64_t serial;                        /* wire identity, %016llX on the wire          */
    char     device_id[FACTORY_STR_MAX];
    char     hw_rev[FACTORY_STR_MAX];
    char     server_host[FACTORY_HOST_MAX];
    uint16_t server_port;
    char     bearer[FACTORY_BEARER_MAX];
    uint8_t  hmac_key[32];
    char     hmac_kid[FACTORY_KID_MAX];
    char     batch[FACTORY_BATCH_MAX];
    char     prod_date[FACTORY_DATE_MAX];
    char     client_cert[FACTORY_PEM_MAX];  /* empty -> embedded g_tls_* fallbacks    */
    char     client_key[FACTORY_PEM_MAX];
} factory_config_t;

/* Call once early in boot (before dp_init/task_com_start). Always returns
 * ESP_OK — missing/corrupt factory data is NOT a boot error, it leads to
 * the fallback identity (from_factory == false). */
esp_err_t factory_config_init(void);

/* Static, process-lifetime instance — pointers into it may be adopted
 * directly into long-lived configurations (fp_config_t). */
const factory_config_t *factory_config_get(void);
