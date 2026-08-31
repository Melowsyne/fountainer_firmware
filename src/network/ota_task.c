/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "ota_task.h"
#include "fountain_proto.h"
#include "wlan_com.h"
#include "task_com.h"
#include "event_manager.h"
#include "link_quality.h"
#include "power_mgmt.h"
#include "debug.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"

#define TAG "ota"

/* Job data copied out of ota_available (the message is gone afterwards). */
typedef struct {
    char strUrl[256];
    char strVersion[32];
    char strSha256[72];
    uint32_t ulSize;
} ota_job_t;

static const char *str_of(const cJSON *m, const char *k)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(m, k);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

/* Checks the SHA-256 of the freshly written update slot against the server-
 * attested hash from ota_available (scope=control). Only this turns pure
 * transmission integrity into real authenticity (Addendum section H). */
static bool ota_verify_sha256(uint32_t ulSize, const char *pstrExpHex)
{
    if (!pstrExpHex || strlen(pstrExpHex) < 64 || ulSize == 0) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "missing/too short sha256 — verification skipped");
        return false;
    }
    const esp_partition_t *pstPart = esp_ota_get_next_update_partition(NULL);
    if (!pstPart || ulSize > pstPart->size) return false;

    uint8_t *pucBuf = malloc(4096);
    if (!pucBuf) return false;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);             /* 0 = SHA-256 (not 224) */

    bool bOk = true;
    for (uint32_t ulOff = 0; ulOff < ulSize; ) {
        uint32_t ulN = (ulSize - ulOff) > 4096 ? 4096 : (ulSize - ulOff);
        if (esp_partition_read(pstPart, ulOff, pucBuf, ulN) != ESP_OK) { bOk = false; break; }
        mbedtls_sha256_update(&ctx, pucBuf, ulN);
        ulOff += ulN;
    }
    free(pucBuf);

    uint8_t aucDigest[32];
    mbedtls_sha256_finish(&ctx, aucDigest);
    mbedtls_sha256_free(&ctx);
    if (!bOk) return false;

    char strHex[65];
    for (int i = 0; i < 32; ++i) snprintf(strHex + 2 * i, 3, "%02x", aucDigest[i]);
    bool bMatch = (strcasecmp(strHex, pstrExpHex) == 0);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "sha256 %s (got=%.12s… want=%.12s…)",
            bMatch ? "OK" : "MISMATCH", strHex, pstrExpHex);
    return bMatch;
}

/* OTA task: streams the image, checks the attested sha256 BEFORE activating
 * the boot slot and flashes; reports ota_status. */
/* Version payload for the EVT_OTA_* events (fixed 16 bytes, NUL-padded). */
static void ota_event_version(system_event_t eEvent, const char *pstrVersion)
{
    char astrVer[16] = {0};
    strncpy(astrVer, pstrVersion, sizeof(astrVer) - 1);
    event_manager_publish(eEvent, astrVer, sizeof(astrVer));
}

static void ota_event_failed(uint8_t ucPhase, esp_err_t err)
{
    evt_ota_failed_t stInfo = { .ucPhase = ucPhase, .slEspErr = (int)err };
    event_manager_publish(EVT_OTA_FAILED, &stInfo, sizeof(stInfo));
}

static void ota_run(void *pvArg)
{
    ota_job_t *pstJob = (ota_job_t *)pvArg;

    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "OTA start v=%s size=%u url=%s",
            pstJob->strVersion, (unsigned)pstJob->ulSize, pstJob->strUrl);
    ota_event_version(EVT_OTA_STARTED, pstJob->strVersion);
    fountain_proto_ota_status_send(pstJob->strVersion, "downloading", 1, 0, NULL, NULL);

    esp_http_client_config_t stHttp = {
        .url               = pstJob->strUrl,
        .timeout_ms        = 30000,
        .keep_alive_enable = true,
        /* https download: CA pinning + mutual TLS with the device cert (the
         * server enforces a client certificate). NULL values (unprovisioned
         * PKI) leave the plaintext-http development fallback intact. */
        .cert_pem          = task_com_tls_ca_get(),
        .client_cert_pem   = task_com_tls_client_cert_get(),
        .client_key_pem    = task_com_tls_client_key_get(),
    };
    esp_https_ota_config_t stOta = { .http_config = &stHttp };

    /* --- Advanced API: stream so we can verify before finish(). --- */
    esp_https_ota_handle_t hOta = NULL;
    esp_err_t err = esp_https_ota_begin(&stOta, &hOta);
    if (err != ESP_OK || hOta == NULL) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "ota_begin failed: %s",
                esp_err_to_name(err));
        fountain_proto_ota_status_send(pstJob->strVersion, "failed", 1, 0,
                                       "begin_failed", esp_err_to_name(err));
        ota_event_failed(0, err);
        goto done;
    }

    int slImageSize = esp_https_ota_get_image_size(hOta);
    do {
        err = esp_https_ota_perform(hOta);
        int slRead = esp_https_ota_get_image_len_read(hOta);
        uint8_t ucPct = (slImageSize > 0) ? (uint8_t)((int64_t)slRead * 100 / slImageSize) : 0;
        logging(LOG_TARGET_AUTO, DBG_LVL_VERBOSE, TAG, "OTA %d/%d (%u%%)",
                slRead, slImageSize, ucPct);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(hOta)) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "Download incomplete: %s",
                esp_err_to_name(err));
        esp_https_ota_abort(hOta);
        fountain_proto_ota_status_send(pstJob->strVersion, "failed", 1, 0,
                                       "download_failed", esp_err_to_name(err));
        ota_event_failed(1, err);
        goto done;
    }

    /* Check the server-attested sha256 against the written slot. */
    if (!ota_verify_sha256(pstJob->ulSize, pstJob->strSha256)) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "sha256 verification failed — discarding image");
        esp_https_ota_abort(hOta);          /* do NOT activate the boot slot */
        fountain_proto_ota_status_send(pstJob->strVersion, "failed", 1, 0,
                                       "sha256_mismatch", NULL);
        ota_event_failed(2, ESP_ERR_INVALID_CRC);
        goto done;
    }

    err = esp_https_ota_finish(hOta);       /* activates the boot slot */
    hOta = NULL;
    if (err == ESP_OK) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "OTA verified & applied -> restart");
        ota_event_version(EVT_OTA_APPLIED, pstJob->strVersion);
        fountain_proto_ota_status_send(pstJob->strVersion, "applied", 1, 100, NULL, NULL);
        vTaskDelay(pdMS_TO_TICKS(500));     /* let ota_status be sent out */
        /* CLEAN WLAN teardown before the reboot: protected deauth + wait for
         * STA_DISCONNECTED, so the AP releases the 802.11w SA immediately.
         * Otherwise the AP keeps the old association and rejects the re-login
         * for minutes after the restart ("comeback time"). Source: esp-idf #9428. */
        wlan_com_teardown_for_reboot(1000);
        esp_restart();
    } else {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "ota_finish failed: %s",
                esp_err_to_name(err));
        fountain_proto_ota_status_send(pstJob->strVersion, "failed", 1, 0,
                                       "finish_failed", esp_err_to_name(err));
        /* finish() is where the RSA signature block gets verified — an
         * unsigned/badly signed image ends up exactly here. */
        ota_event_version(EVT_OTA_REJECTED_UNSIGNED, pstJob->strVersion);
    }

done:
    /* Every path that ends here did NOT reboot -> hand the session over to
     * normal operation (otherwise it stays in the negotiated-but-not-running
     * limbo until the 180-s session watchdog recovers it). */
    fountain_proto_ota_failed_note();
    free(pstJob);
    vTaskDelete(NULL);
}

void ota_start(const cJSON *pstOtaMsg)
{
    /* Link gate (Link_Robustness_v1 §B4): esp_https_ota cannot RESUME — a
     * 1.7-MB download on a poor link burns airtime just to fail. Refuse
     * now; the server re-offers automatically on the next (re)connect. */
    if (link_quality_poor_get()) {
        const char *pstrVer = str_of(pstOtaMsg, "version");
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "OTA deferred: link quality POOR (re-offer on next connect)");
        fountain_proto_ota_status_send(pstrVer ? pstrVer : "?", "failed", 1, 0,
                                       "link_poor", NULL);
        fountain_proto_ota_failed_note();
        return;
    }

    power_mgmt_activity_note();   /* OTA = active task: full clock, no PS */
    const char *pstrUrl = str_of(pstOtaMsg, "url");
    if (!pstrUrl) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "ota_available without url — ignored");
        fountain_proto_ota_failed_note();
        return;
    }

    ota_job_t *pstJob = calloc(1, sizeof(*pstJob));
    if (!pstJob) { fountain_proto_ota_failed_note(); return; }

    const char *pstrVer = str_of(pstOtaMsg, "target_version");
    const char *pstrSha = str_of(pstOtaMsg, "sha256");
    const cJSON *pSize  = cJSON_GetObjectItemCaseSensitive(pstOtaMsg, "size");

    snprintf(pstJob->strUrl,     sizeof pstJob->strUrl,     "%s", pstrUrl);
    snprintf(pstJob->strVersion, sizeof pstJob->strVersion, "%s", pstrVer ? pstrVer : "");
    snprintf(pstJob->strSha256,  sizeof pstJob->strSha256,  "%s", pstrSha ? pstrSha : "");
    pstJob->ulSize = cJSON_IsNumber(pSize) ? (uint32_t)pSize->valuedouble : 0;

    /* Own task — the session must not block. */
    if (xTaskCreate(ota_run, "ota", 8192, pstJob, 4, NULL) != pdPASS) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "OTA task start failed");
        free(pstJob);
        fountain_proto_ota_failed_note();
    }
}
