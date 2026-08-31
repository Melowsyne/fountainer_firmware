/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * factory_config — reads the production NVS partition, see header.
 */
#include "factory_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "factory";

static factory_config_t s_stFactory;

/* Bit-exact the previous testbed identity (formerly hard-coded in
 * task_com.c:388ff and main.c:363) — behavior without a factory partition
 * stays identical to the existing firmware. */
static void defaults_set(void)
{
    memset(&s_stFactory, 0, sizeof s_stFactory);
    s_stFactory.serial = 0x000001C0C01FA82AULL;
    strlcpy(s_stFactory.device_id, "esp32-a1b2c3d4e5f6", sizeof s_stFactory.device_id);
    strlcpy(s_stFactory.hw_rev, "0.10.0", sizeof s_stFactory.hw_rev);
    strlcpy(s_stFactory.server_host, "192.168.1.12", sizeof s_stFactory.server_host);
    s_stFactory.server_port = 8443;
    strlcpy(s_stFactory.bearer, "testbed-bearer-token-rotate-me", sizeof s_stFactory.bearer);
    /* Testbed HMAC key (golden vector); production: per device from the
     * factory partition. TODO production hardening: flash-encrypted NVS
     * or HMAC eFuse block. */
    for (int i = 0; i < 32; i++) s_stFactory.hmac_key[i] = (uint8_t)i;
    strlcpy(s_stFactory.hmac_kid, "1", sizeof s_stFactory.hmac_kid);
    /* client_cert/client_key empty -> the embedded certs_gen.c symbols apply */
}

static bool get_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    return nvs_get_str(h, key, dst, &len) == ESP_OK;
}

esp_err_t factory_config_init(void)
{
    defaults_set();

    esp_err_t e = nvs_flash_init_partition(FACTORY_PART_LABEL);
    if (e != ESP_OK) {
        /* Partition missing (old table) or corrupt: NEVER erase — the
         * production data is not reproducible. The fallback applies. */
        ESP_LOGW(TAG, "factory partition unavailable (%s) -> builtin identity",
                 esp_err_to_name(e));
        return ESP_OK;
    }

    nvs_handle_t h;
    if (nvs_open_from_partition(FACTORY_PART_LABEL, FACTORY_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "factory partition blank -> builtin identity");
        return ESP_OK;
    }

    /* Identity/security block all-or-nothing: half an identity is worse
     * than none (the device would report with mixed data). */
    uint64_t ullSerial;
    char acId[FACTORY_STR_MAX], acKid[FACTORY_KID_MAX], acBearer[FACTORY_BEARER_MAX];
    uint8_t aucKey[32];
    size_t n = sizeof aucKey;
    bool ok = nvs_get_u64(h, "serial", &ullSerial) == ESP_OK
           && get_str(h, "device_id", acId, sizeof acId)
           && get_str(h, "hmac_kid", acKid, sizeof acKid)
           && get_str(h, "bearer", acBearer, sizeof acBearer)
           && nvs_get_blob(h, "hmac_key", aucKey, &n) == ESP_OK && n == 32;
    if (!ok) {
        ESP_LOGE(TAG, "factory data incomplete -> builtin identity");
        nvs_close(h);
        return ESP_OK;
    }

    s_stFactory.serial = ullSerial;
    strlcpy(s_stFactory.device_id, acId, sizeof s_stFactory.device_id);
    strlcpy(s_stFactory.hmac_kid, acKid, sizeof s_stFactory.hmac_kid);
    strlcpy(s_stFactory.bearer, acBearer, sizeof s_stFactory.bearer);
    memcpy(s_stFactory.hmac_key, aucKey, 32);

    /* Optional individual values — a missing one keeps its default. */
    get_str(h, "hw_rev", s_stFactory.hw_rev, sizeof s_stFactory.hw_rev);
    get_str(h, "server", s_stFactory.server_host, sizeof s_stFactory.server_host);
    uint16_t usPort;
    if (nvs_get_u16(h, "server_port", &usPort) == ESP_OK && usPort != 0)
        s_stFactory.server_port = usPort;
    get_str(h, "batch", s_stFactory.batch, sizeof s_stFactory.batch);
    get_str(h, "prod_date", s_stFactory.prod_date, sizeof s_stFactory.prod_date);

    /* Certificate + key only as a PAIR — with only one present both stay
     * empty and the embedded fallbacks apply (else mTLS with half material).
     * Read directly into the static instance (3 KB buffers do not belong on
     * the init task's stack); reset on an incomplete pair. */
    if (!(get_str(h, "client_cert", s_stFactory.client_cert, sizeof s_stFactory.client_cert) &&
          get_str(h, "client_key",  s_stFactory.client_key,  sizeof s_stFactory.client_key) &&
          s_stFactory.client_cert[0] && s_stFactory.client_key[0])) {
        s_stFactory.client_cert[0] = '\0';
        s_stFactory.client_key[0]  = '\0';
    }

    s_stFactory.from_factory = true;
    nvs_close(h);
    ESP_LOGI(TAG, "factory identity loaded: %s (serial %016llX)",
             s_stFactory.device_id, (unsigned long long)s_stFactory.serial);
    return ESP_OK;
}

const factory_config_t *factory_config_get(void)
{
    return &s_stFactory;
}
