/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * local_protocol — Fountain v2.2 over the local WSS transport
 * (firmware_server.md §4/§5, implementation plan, work package AP5).
 *
 * The ESP32 remains the Fountain DEVICE: for every accepted connection IT
 * sends hello; the maintenance client (Fountain SERVER role) answers hello_ack,
 * verifies the signed ota_check proof and switches to RUNNING with ota_none
 * (§48 variant A — zero protocol change).
 *
 * CONTROL PARITY: on_dp_write/on_command/fill_log_batch/on_log_ack_prev/
 * fill_history_batch are
 * set and call the SHARED cloud application logic (task_com_apply_*) —
 * identical feature set to the web UI. The isolation contract is preserved:
 * the task_com_apply_* variants call NO power_mgmt_activity_note()
 * (local sessions do not keep the device in HIGH-power mode); on_ready/
 * on_session_lost/on_alive/on_ota/fill_changes stay NULL — local sessions
 * touch neither cloud events (config backup!) nor the WD_SESSION watchdog
 * nor the on-change baseline.
 */
#include <stdio.h>

#include "esp_app_desc.h"

#include "datapoints.h"
#include "debug.h"
#include "factory_config.h"
#include "fp_session.h"
#include "local_server.h"
#include "task_com.h"

static const char *TAG = "local_proto";

#define LOCAL_JSON_STRIKES_MAX 3      /* invalid messages -> close (§33) */

/* ----- Callbacks --------------------------------------------------------- */
static void cb_local_fill_snapshot(cJSON *dp_out, const cJSON *names, void *user)
{
    (void)user;
    dp_refresh();
    if (names && cJSON_GetArraySize((cJSON *)names) > 0)
        dp_read_into(names, dp_out, NULL);
    else
        dp_report_full_noreset(dp_out);   /* cloud baseline untouched */
}

/* dp_write/command/log_read: shared cloud logic (no power_note). Thin
 * wrappers because of the extra user parameter of the fp_config callbacks. */
static void cb_local_on_dp_write(const cJSON *dp, cJSON *result_out, void *user)
{
    (void)user;
    task_com_apply_dp_write(dp, result_out);
}

static void cb_local_on_command(const cJSON *cmd_msg, cJSON *result_out, void *user)
{
    (void)user;
    task_com_apply_command(cmd_msg, result_out);
}

/* ----- local fp_config_t (one instance for both slots) ------------------- */
static fp_config_t s_stLocalCfg;          /* long-lived — sessions reference it */
static char        s_acSerialHex[17];

static const fp_config_t *local_cfg_get(void)
{
    if (!s_stLocalCfg.device_id) {
        const factory_config_t *fc = factory_config_get();
        snprintf(s_acSerialHex, sizeof s_acSerialHex, "%016llX",
                 (unsigned long long)fc->serial);
        s_stLocalCfg.device_id     = fc->device_id;
        s_stLocalCfg.serial        = s_acSerialHex;
        s_stLocalCfg.bearer_token  = "";            /* no bearer locally (plan) */
        s_stLocalCfg.auth_kid      = fc->hmac_kid;
        s_stLocalCfg.auth_key      = fc->hmac_key;
        s_stLocalCfg.auth_key_len  = 32;
        s_stLocalCfg.fw_version    = esp_app_get_description()->version;
        s_stLocalCfg.hw_rev        = fc->hw_rev;
        s_stLocalCfg.heartbeat_s   = 0;             /* no periodic tick        */
        s_stLocalCfg.report_s      = 0;
        s_stLocalCfg.fill_snapshot = cb_local_fill_snapshot;
        /* CONTROL parity: writes/commands/log pull like the cloud. */
        s_stLocalCfg.on_dp_write     = cb_local_on_dp_write;
        s_stLocalCfg.on_command      = cb_local_on_command;
        s_stLocalCfg.fill_log_batch  = task_com_fill_log_batch;
        s_stLocalCfg.on_log_ack_prev = task_com_log_ack_prev;
        s_stLocalCfg.fill_history_batch = task_com_fill_history_batch;
        /* Deliberately NULL: on_ready/on_session_lost/on_alive/on_ota/fill_changes
         * (see header comment — cloud isolation). */
    }
    return &s_stLocalCfg;
}

/* ----- Transport adapter -------------------------------------------------- */
static bool local_send_adapter(void *ctx, const char *json)
{
    return local_server_session_send((local_session_t *)ctx, json);
}

/* ----- Hooks from local_server.c (override the weak defaults) ------------- */
void local_protocol_on_open(local_session_t *s)
{
    fp_session_init(&s->protocol, local_cfg_get(), local_send_adapter, s);
    fp_session_reset(&s->protocol);
    s->json_strikes = 0;
    if (!fp_session_hello_send(&s->protocol))
        local_server_session_close(s, LOCAL_CLOSE_PROTOCOL_ERROR);
}

void local_protocol_on_frame(local_session_t *s, const char *json)
{
    if (!fp_session_message_handle(&s->protocol, json)) {
        if (++s->json_strikes >= LOCAL_JSON_STRIKES_MAX) {
            local_server_session_close(s, LOCAL_CLOSE_PROTOCOL_ERROR);
            return;
        }
    }
    if (fp_session_running(&s->protocol) && s->state == LOCAL_SESSION_CONNECTED) {
        s->state = LOCAL_SESSION_RUNNING;
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "local session running (conn %u)", (unsigned)s->connection_id);
    }
}

void local_protocol_on_close(local_session_t *s)
{
    /* fp_session_close() (zeroization) is done by local_server.c in cleanup. */
    logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG,
            "local session closed (conn %u, running=%d)",
            (unsigned)s->connection_id, (int)fp_session_running(&s->protocol));
}
