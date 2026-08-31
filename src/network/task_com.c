/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "task_com.h"
#include "factory_config.h"
#include "ota_task.h"
#include "wlan_com.h"
#include "command.h"
#include "network_config.h"
#include "link_quality.h"
#include "power_mgmt.h"
#include "datapoints.h"
#include "event_manager.h"
#include "logging.h"
#include "pressure_history.h"
#include "debug.h"
#include "fountain_proto.h"

#include "cJSON.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

#define TAG "task_com"

/* =============================================================
 * task_com — binding to the reusable component
 * `clientside_protocol` (Fountain v2.2). The component runs
 * WebSocket, handshake/negotiation, HMAC auth (scope=control),
 * heartbeat/dp_report and the dispatching. This module provides
 * only the integration callbacks (datapoints/commands/OTA) and
 * starts the component as soon as WLAN is connected.
 * ============================================================= */

/* --- Device identity ---------------------------------------------------------
 * Series production: serial, device_id, bearer, HMAC key/kid, server fallback
 * and (in production images) the client certificate come from factory_config
 * (factory NVS partition). Without a factory partition, factory_config
 * provides the legacy testbed identity as compiled-in fallback — the wire
 * behaviour is byte-identical to the previous hardcoded values. */

/* TLS material, generated into src/certs_gen.c from src/certs/ (testbed PKI
 * ../DO_NOT_COMMIT/CA) by tools/ensure_network_json.py. Empty strings when
 * the PKI files are absent (or in esp32s3_prod builds, which strip the
 * per-device client material) -> factory partition or plaintext fallback. */
extern const char g_tls_ca_pem[];
extern const char g_tls_client_cert_pem[];
extern const char g_tls_client_key_pem[];

/* Also used by ota_task.c for the https firmware download. Client material is
 * factory-first: a provisioned device uses its own certificate; the embedded
 * PEMs remain as testbed/legacy fallback. The CA is common to all devices
 * and stays embedded. */
const char *task_com_tls_ca_get(void)          { return g_tls_ca_pem[0]          ? g_tls_ca_pem          : NULL; }
const char *task_com_tls_client_cert_get(void)
{
    const factory_config_t *fc = factory_config_get();
    if (fc->client_cert[0]) return fc->client_cert;
    return g_tls_client_cert_pem[0] ? g_tls_client_cert_pem : NULL;
}
const char *task_com_tls_client_key_get(void)
{
    const factory_config_t *fc = factory_config_get();
    if (fc->client_key[0]) return fc->client_key;
    return g_tls_client_key_pem[0] ? g_tls_client_key_pem : NULL;
}

#define SERVER_PATH "/ws"

static fp_config_t s_stCfg;          /* long-lived — referenced by the component */
static bool        s_bStarted = false;

/* ----- Integration callbacks -------------------------------------------- */

/* Fill datapoint snapshot (full snapshot or requested names). */
static void cb_fill_snapshot(cJSON *dp_out, const cJSON *names, void *user)
{
    (void)user;
    /* A NAMED dp_read is an active server request (the periodic report and
     * the full-snapshot variant pass names == NULL/empty). */
    if (names && cJSON_IsArray(names) && cJSON_GetArraySize(names) > 0)
        power_mgmt_activity_note();
    dp_refresh();
    if (names && cJSON_IsArray(names) && cJSON_GetArraySize(names) > 0)
        dp_read_into(names, dp_out, NULL);
    else
        dp_report_full(dp_out);
}

/* Verified command -> existing command module (COMMAND/FACADE).
 * Shared application logic WITHOUT power_mgmt_activity_note — so the local
 * maintenance session (local_protocol) can use exactly the same semantics
 * without pinning the device in HIGH-power mode (cloud wrapper below). */
void task_com_apply_command(const cJSON *cmd_msg, cJSON *result_out)
{
    const cJSON *pCmd  = cJSON_GetObjectItemCaseSensitive(cmd_msg, "command");
    const cJSON *pTgt  = cJSON_GetObjectItemCaseSensitive(cmd_msg, "target_state");
    const cJSON *pDur  = cJSON_GetObjectItemCaseSensitive(cmd_msg, "duration_steps");
    const char *pstrCmd = cJSON_IsString(pCmd) ? pCmd->valuestring : NULL;
    const char *pstrTgt = cJSON_IsString(pTgt) ? pTgt->valuestring : NULL;
    uint32_t    ulSteps = cJSON_IsNumber(pDur) ? (uint32_t)pDur->valuedouble : 0;

    /* Network-layer test command (intercepted HERE, not in the device
     * command module — the device layer must not know link_quality):
     * link_fault forces POOR for <duration_steps> seconds. */
    if (pstrCmd && strcmp(pstrCmd, "link_fault") == 0) {
        link_quality_test_poor_set(ulSteps ? ulSteps : 60);
        cJSON_AddStringToObject(result_out, "status", "applied");
        return;
    }

    command_t        stCmd;
    command_result_t stRes;
    /* Distinguish the error cause (formerly everything was "unknown_command" —
     * misleading when a KNOWN command merely fails at the device logic,
     * e.g. manual switch-on while a pump fault is latched). */
    if (!pstrCmd || !command_protocol_map(pstrCmd, pstrTgt, ulSteps, &stCmd)) {
        cJSON_AddStringToObject(result_out, "status", "rejected");
        cJSON_AddStringToObject(result_out, "error", "unknown_command");
    } else if (!command_execute(&stCmd, &stRes) || !stRes.bOk) {
        cJSON_AddStringToObject(result_out, "status", "rejected");
        cJSON_AddStringToObject(result_out, "error", "not_permitted");
    } else {
        cJSON_AddStringToObject(result_out, "status", "applied");
    }
}

/* Cloud wrapper: an active server request keeps the device awake. */
static void cb_on_command(const cJSON *cmd_msg, cJSON *result_out, void *user)
{
    (void)user;
    power_mgmt_activity_note();
    task_com_apply_command(cmd_msg, result_out);
}

/* Verified dp_write -> atomic batch write via the datapoints API.
 * Network_Save is a COMMAND point (VOLATILE, outside the NVS region) and is
 * intercepted here: it is stripped from the batch and executed AFTER the
 * remaining points were applied (so "write config + Network_Save" in one
 * message behaves as expected). */
static void log_command_handle(uint8_t ucCmd);

/* Shared dp_write application WITHOUT power_mgmt_activity_note (see above). */
void task_com_apply_dp_write(const cJSON *dp, cJSON *result_out)
{
    uint8_t ucSave = 0;
    cJSON  *pBatch = cJSON_Duplicate(dp, true);
    const cJSON *pSave = cJSON_GetObjectItemCaseSensitive(pBatch, "Network_Save");
    if (pSave) {
        if (cJSON_IsNumber(pSave)) ucSave = (uint8_t)pSave->valuedouble;
        cJSON_DeleteItemFromObjectCaseSensitive(pBatch, "Network_Save");
    }
    /* Log_Command is a volatile command point as well — execute + strip. */
    const cJSON *pLogCmd = cJSON_GetObjectItemCaseSensitive(pBatch, "Log_Command");
    if (pLogCmd) {
        if (cJSON_IsNumber(pLogCmd))
            log_command_handle((uint8_t)pLogCmd->valuedouble);
        cJSON_DeleteItemFromObjectCaseSensitive(pBatch, "Log_Command");
    }

    cJSON *pErrs = cJSON_CreateObject();
    bool bPersisted = true;
    bool bOk = dp_write_batch(pBatch, pErrs, &bPersisted);
    cJSON_Delete(pBatch);
    if (bOk && !bPersisted) {
        /* Applied in RAM, but the NVS save failed — the server must not
         * mistake this for a durable change. */
        cJSON_AddStringToObject(result_out, "warning", "nvs_save_failed");
        LOG_EMIT0(LOG_LEVEL_ERROR, LOG_MOD_SYSTEM, LOG_EVT_NVS_SAVE_FAIL,
                  "dp config in RAM only");
    }

    if (bOk && ucSave != 0) {
        if (!network_config_save_handle(ucSave)) {
            cJSON_AddStringToObject(result_out, "status", "rejected");
            cJSON_AddStringToObject(result_out, "error", "invalid_network_save");
            cJSON_Delete(pErrs);
            return;
        }
        cJSON_AddNumberToObject(result_out, "network_save", ucSave);
    }
    if (bOk && cJSON_GetArraySize(pErrs) == 0) {
        uint8_t ucCount = (uint8_t)cJSON_GetArraySize(dp);
        event_manager_publish(EVT_DP_WRITTEN, &ucCount, sizeof(ucCount));
        cJSON_AddStringToObject(result_out, "status", "applied");
        cJSON_Delete(pErrs);
        /* Readback of the written keys -> round-trip verifiable server-side. */
        cJSON *pNames = cJSON_CreateArray();
        for (const cJSON *it = dp->child; it; it = it->next)
            if (it->string) cJSON_AddItemToArray(pNames, cJSON_CreateString(it->string));
        cJSON *pRb = cJSON_AddObjectToObject(result_out, "readback");
        dp_read_into(pNames, pRb, NULL);
        cJSON_Delete(pNames);
    } else {
        cJSON_AddStringToObject(result_out, "status", "rejected");
        cJSON_AddItemToObject(result_out, "errors", pErrs);  /* takes ownership of pErrs */
    }
}

/* Cloud wrapper: an active server request keeps the device awake. */
static void cb_on_dp_write(const cJSON *dp, cJSON *result_out, void *user)
{
    (void)user;
    power_mgmt_activity_note();
    task_com_apply_dp_write(dp, result_out);
}

/* Verified ota_available -> start OTA task (do not block!). */
static void cb_on_ota(const cJSON *ota_msg, void *user)
{
    (void)user;
    ota_start(ota_msg);
}

/* Link probe for the OTA/https paths and diagnostics. */
static bool cb_link_up(void *user)
{
    (void)user;
    return wlan_com_connected_get();
}

/* On-change report source: VOLATILE points that flipped (bool/enum) or
 * moved past their deadband (F32) since the last report — activates the
 * dp_report_changes machinery that existed unused until now. */
static int cb_fill_changes(cJSON *dp_out, void *user)
{
    (void)user;
    return dp_report_changes(dp_out);
}

/* Session progress -> injected hook (main feeds the WD_SESSION channel). */
static void (*s_pfnAliveHook)(void);
void task_com_alive_hook_set(void (*pfnAlive)(void)) { s_pfnAliveHook = pfnAlive; }

static void cb_on_alive(void *user)
{
    (void)user;
    if (s_pfnAliveHook) s_pfnAliveHook();
}

/* log_read/log_read_prev -> log_batch body from the logging component.
 * Records go compact on the wire: {s,u,ev,mod,lvl,a[],t}.
 * LOG_BATCH_BYTE_BUDGET: hard cap on the SERIALIZED size — a full ring of
 * long-text records at 64/128 records blew past the server's former 8-KiB
 * websocket frame limit, which killed the connection on every first poll
 * (endless session loop, found live 2026-07-09). Fewer records per batch
 * are always legal: the server keeps polling via since_seq. */
#define LOG_BATCH_MAX 128
#define LOG_BATCH_BYTE_BUDGET       24000
/* On a POOR link small frames have drastically better odds against packet
 * loss — the server simply re-polls more often via since_seq (§B2). */
#define LOG_BATCH_BYTE_BUDGET_POOR   4000
/* Public so that the local maintenance session uses the same log-pull logic
 * (no cloud-specific state). Signature = fp_config callback. */
void task_com_fill_log_batch(cJSON *body_out, uint32_t since_seq,
                             uint8_t min_level, uint16_t max_records,
                             bool prev_boot, void *user)
{
    (void)user;
    if (max_records > LOG_BATCH_MAX) max_records = LOG_BATCH_MAX;

    /* Heap, not stack: 128 records ≈ 11 KB (task stack is 4-6 KB). */
    log_record_t *pstRecs = malloc(sizeof(log_record_t) * max_records);
    size_t szN = 0;
    log_stats_t stStats;

    if (prev_boot) {
        uint32_t ulPrevBoot = 0;
        bool bAvail = logging_previous_boot_available(&ulPrevBoot);
        szN = (bAvail && pstRecs)
                  ? logging_read_previous_boot((log_level_t)min_level,
                                               pstRecs, max_records) : 0;
        logging_stats_get(&stStats);
        cJSON_AddNumberToObject(body_out, "boot_id", ulPrevBoot);
        cJSON_AddBoolToObject(body_out, "available", bAvail);
    } else {
        szN = pstRecs ? logging_read_since(since_seq, (log_level_t)min_level,
                                           pstRecs, max_records, &stStats)
                      : (logging_stats_get(&stStats), 0);
        cJSON_AddNumberToObject(body_out, "boot_id", stStats.ulBootId);
        cJSON_AddNumberToObject(body_out, "first_seq_available",
                                stStats.ulFirstSeqAvailable);
        cJSON_AddNumberToObject(body_out, "next_seq", stStats.ulNextSeq);
        cJSON_AddNumberToObject(body_out, "dropped_count", stStats.ulDropped);
        /* Overflow: the server's read position fell out of the ring. */
        bool bOverflow = since_seq + 1 < stStats.ulFirstSeqAvailable;
        cJSON_AddBoolToObject(body_out, "overflow", bOverflow);
    }

    cJSON *pRecords = cJSON_AddArrayToObject(body_out, "records");
    size_t szBudget = link_quality_poor_get() ? LOG_BATCH_BYTE_BUDGET_POOR
                                              : LOG_BATCH_BYTE_BUDGET;
    size_t szBytes = 0;
    for (size_t i = 0; pstRecs && i < szN; i++) {
        const log_record_t *r = &pstRecs[i];
        /* Serialized-size estimate; stop before the frame gets oversized
         * (the server re-polls the remainder via since_seq). */
        szBytes += 64 + strlen(r->strText) + (size_t)r->ucArgc * 12;
        if (szBytes > szBudget) break;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "s",   r->ulSeq);
        cJSON_AddNumberToObject(o, "u",   r->ulUptimeMs);
        cJSON_AddNumberToObject(o, "ev",  r->usEventId);
        cJSON_AddNumberToObject(o, "mod", r->ucModuleId);
        cJSON_AddNumberToObject(o, "lvl", r->ucLevel);
        if (r->ucArgc) {
            cJSON *a = cJSON_AddArrayToObject(o, "a");
            for (uint8_t k = 0; k < r->ucArgc; k++)
                cJSON_AddItemToArray(a, cJSON_CreateNumber(r->aslArgs[k]));
        }
        if (r->strText[0]) cJSON_AddStringToObject(o, "t", r->strText);
        cJSON_AddItemToArray(pRecords, o);
    }
    free(pstRecs);
}

bool task_com_log_ack_prev(uint32_t boot_id, void *user)
{
    (void)user;
    return logging_ack_previous_boot(boot_id);
}

/* history_batch body (drucksensor_datenstruktur.md §22): samples as compact
 * [seq,ts_ms,mbar,status] arrays plus cursor/gap metadata. Integers only
 * (cJSON reprint vs. MAC canonicalization). now_ms = device uptime as the
 * anchor for the server's wall-clock mapping; boot_id for reboot detection
 * (the ring is RAM-only). Cloud + local maintenance session share this function. */
void task_com_fill_history_batch(cJSON *body_out, uint32_t since_seq,
                                 uint32_t max_samples, void *user)
{
    (void)user;
    if (max_samples > 120) max_samples = 120;   /* local 4-KB frame limit   */

    /* As in the log path: scratch on the heap, not on the task stack. */
    pressure_sample_t *pstSamples = malloc(sizeof(*pstSamples) * max_samples);
    pressure_history_stats_t stStats;
    size_t szN = 0;
    if (pstSamples)
        szN = pressure_history_read_since(since_seq, pstSamples, max_samples,
                                          &stStats);
    else
        pressure_history_stats_get(&stStats);

    log_stats_t stLog;
    logging_stats_get(&stLog);                  /* reuse the boot_id       */

    cJSON_AddNumberToObject(body_out, "boot_id", stLog.ulBootId);
    cJSON_AddNumberToObject(body_out, "now_ms",
                            (double)(uint32_t)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(body_out, "sample_interval_ms",
                            PRESSURE_HISTORY_INTERVAL_MS);
    cJSON_AddNumberToObject(body_out, "next_seq", stStats.next_seq);
    cJSON_AddNumberToObject(body_out, "first_seq_available",
                            stStats.first_seq_available);
    cJSON_AddNumberToObject(body_out, "overwritten", stStats.overwritten);
    cJSON_AddNumberToObject(body_out, "high_watermark", stStats.high_watermark);

    cJSON *pSamples = cJSON_AddArrayToObject(body_out, "samples");
    for (size_t i = 0; i < szN; i++) {
        const pressure_sample_t *s = &pstSamples[i];
        cJSON *pRow = cJSON_CreateArray();
        cJSON_AddItemToArray(pRow, cJSON_CreateNumber(s->sequence));
        cJSON_AddItemToArray(pRow, cJSON_CreateNumber(s->timestamp_ms));
        cJSON_AddItemToArray(pRow, cJSON_CreateNumber(s->pressure_mbar));
        cJSON_AddItemToArray(pRow, cJSON_CreateNumber(s->status));
        cJSON_AddItemToArray(pSamples, pRow);
    }
    free(pstSamples);
}

/* Log_Command interception (like Network_Save): 1=clear ring, 2=ack prev
 * boot, 3=force flush (no-op until the flash tier exists). */
static void log_command_handle(uint8_t ucCmd)
{
    switch (ucCmd) {
    case 1: logging_clear_runtime(); break;
    case 2: (void)logging_ack_previous_boot(0); break;
    case 3: /* flush: the flash task drains its queue continuously —
             * nothing to force; kept for wire compatibility. */ break;
    default: break;
    }
}

static void cb_on_ready(void *user)
{
    (void)user;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "protocol ready (negotiated, running)");
    event_manager_publish(EVT_SESSION_READY, NULL, 0);
    /* The known-good network backup (network_config_backup) subscribes to
     * EVT_SESSION_READY in main — no direct coupling here anymore. */
    power_mgmt_activity_note();          /* fresh session: stay HIGH for a while */
}

/* Session dropped after having been up (edge from the protocol task). */
static void cb_on_session_lost(void *user)
{
    (void)user;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "protocol session lost");
    event_manager_publish(EVT_SESSION_LOST, NULL, 0);
}

static SemaphoreHandle_t s_hWlanSem;   /* signals the starter: WLAN connected */

/* ----- WLAN gate -------------------------------------------------------------
 * IMPORTANT: This callback runs in the sys_evt task (small stack). Do NOT
 * call fountain_proto_start() here — it initializes WS client/mbedTLS/tasks
 * and blows the event task stack (stack overflow). Instead only wake the
 * starter task, which brings up the component in its own context. */
static void com_wlan_state_handle(wlan_state_t eSt, void *pvCtx)
{
    (void)pvCtx;
    if (eSt == WLAN_CONNECTED && !s_bStarted && s_hWlanSem)
        xSemaphoreGive(s_hWlanSem);
}

/* Starter task: waits for WLAN connect and then brings up the protocol. */
static void com_starter_task(void *pvArg)
{
    (void)pvArg;
    for (;;) {
        if (xSemaphoreTake(s_hWlanSem, portMAX_DELAY) == pdTRUE && !s_bStarted) {
            s_bStarted = fountain_proto_start(&s_stCfg);
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "fountain_proto_start=%d", (int)s_bStarted);
        }
    }
}

bool task_com_start(void)
{
    const esp_app_desc_t *pstAppDesc = esp_app_get_description();
    const factory_config_t *pstFc = factory_config_get();

    /* Fill the configuration once (long-lived). */
    /* Server address (IP or DNS name; lwIP resolves names at connect time)
     * from the NVS datapoints (network.json/provisioning), else the factory
     * fallback (factory partition or, without one, the legacy 192.168.1.12).
     * DP_REF returns long-lived storage in g_dp_store -> usable as const char*. */
    const char *pstrHost = DP_REF(Network_Server);
    uint16_t    usPort   = DP_REF(Network_Server_Port);
    if (pstrHost && pstrHost[0]) {
        s_stCfg.server_host = pstrHost;
        s_stCfg.server_port = usPort ? usPort : pstFc->server_port;
    } else {
        /* Empty host means: use the complete factory fallback.
         * This way legacy devices no longer keep using an old port left in NVS
         * (e.g. 8010) when the host was never provisioned. */
        s_stCfg.server_host = pstFc->server_host;
        s_stCfg.server_port = pstFc->server_port;
    }
    s_stCfg.server_path  = SERVER_PATH;
    /* TLS: CA pinning + mutual TLS via the factory/embedded device certificate.
     * With CA present the component speaks wss://; without -> ws:// (dev). */
    s_stCfg.ca_pem          = task_com_tls_ca_get();
    s_stCfg.client_cert_pem = task_com_tls_client_cert_get();
    s_stCfg.client_key_pem  = task_com_tls_client_key_get();
    /* Wire identity from factory_config — the SAME source main.c feeds into
     * Device_Serial_Number, so datapoint and envelope serial always match
     * (%016llX). All factory_config pointers are process-lifetime static. */
    static char s_acSerialHex[17];
    snprintf(s_acSerialHex, sizeof s_acSerialHex, "%016llX",
             (unsigned long long)pstFc->serial);
    s_stCfg.device_id    = pstFc->device_id;
    s_stCfg.serial       = s_acSerialHex;
    s_stCfg.bearer_token = pstFc->bearer;
    s_stCfg.auth_kid     = pstFc->hmac_kid;
    s_stCfg.auth_key     = pstFc->hmac_key;
    s_stCfg.auth_key_len = 32;
    s_stCfg.fw_version   = pstAppDesc->version;
    /* Legacy wire value "esp32s3" without a factory partition (byte-identical
     * hello/ota_check for the existing device); provisioned devices report
     * their real hardware revision. */
    s_stCfg.hw_rev       = pstFc->from_factory ? pstFc->hw_rev : "esp32s3";
    s_stCfg.heartbeat_s  = 30;
    s_stCfg.report_s     = 10;
    s_stCfg.fill_snapshot = cb_fill_snapshot;
    s_stCfg.on_command    = cb_on_command;
    s_stCfg.on_dp_write   = cb_on_dp_write;
    s_stCfg.on_ready      = cb_on_ready;
    s_stCfg.on_ota        = cb_on_ota;
    s_stCfg.link_up       = cb_link_up;   /* connectivity watchdog probe */
    s_stCfg.fill_log_batch  = task_com_fill_log_batch;   /* logging pull (WP 3) */
    s_stCfg.on_log_ack_prev = task_com_log_ack_prev;
    s_stCfg.fill_history_batch = task_com_fill_history_batch; /* pressure history */
    s_stCfg.on_alive        = cb_on_alive;         /* WD_SESSION heartbeat */
    s_stCfg.on_session_lost = cb_on_session_lost;
    s_stCfg.fill_changes    = cb_fill_changes;     /* on-change dp_report  */
    s_stCfg.user          = NULL;

    /* Create starter task + signal, then couple to WLAN connect. This way the
     * protocol start runs outside the sys_evt task (enough stack). */
    s_hWlanSem = xSemaphoreCreateBinary();
    if (!s_hWlanSem) return false;
    if (xTaskCreatePinnedToCore(com_starter_task, "com_starter", 6144,
                                NULL, 5, NULL, 1) != pdPASS)
        return false;

    wlan_com_state_cb_set(com_wlan_state_handle, NULL);
    if (wlan_com_connected_get())
        xSemaphoreGive(s_hWlanSem);

    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "task_com ready (target ws://%s:%d%s, starts on WLAN connect)",
            s_stCfg.server_host, s_stCfg.server_port, s_stCfg.server_path);
    return true;
}
