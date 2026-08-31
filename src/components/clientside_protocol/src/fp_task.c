/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "fp_task.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "fp_envelope.h"
#include "fp_session.h"
#include "fp_ws.h"
#include "fountain_msgs.h"

static const char *TAG = "fp_task";

#define FP_TXQ_DEPTH 8   /* outgoing frames in flight (see tx_task) */

typedef struct {
    const fp_config_t *cfg;
    fp_ws_t           *ws;
    fp_session_t       session;
    SemaphoreHandle_t  lock;
    TaskHandle_t       task;
    QueueHandle_t      txq;      /* char* items (malloc'd JSON frames) */
    TaskHandle_t       tx_task;
} fp_ctx_t;

static fp_ctx_t s_ctx;

/* Slow mode (power saving): stretch heartbeat AND dp_report to the same 60 s
 * grid -> the WiFi modem only has to wake for one TX window per minute.
 * The server's offline watchdog must be >= 150 s for this (see
 * FOUNTAIN_HEARTBEAT_TIMEOUT_MS on the server side). */
#define FP_SLOW_INTERVAL_S 60
static volatile bool s_slow_mode = false;

void fountain_proto_slow_mode_set(bool slow) { s_slow_mode = slow; }

bool fountain_proto_running(void) { return fp_session_running(&s_ctx.session); }

/* Stage-1 recovery action for the app_watchdog's WD_SESSION channel:
 * hard-restart the WS client (tears down + recreates the connection). */
bool fountain_proto_recover(void)
{
    if (!s_ctx.ws) return false;
    fp_ws_restart(s_ctx.ws);
    return true;
}

/* Unix ms once the time (SNTP) is set; otherwise 0 (spec allows ts=0). */
int64_t fp_now_ms(void) {
    time_t t = time(NULL);
    if (t < 1600000000) return 0;        /* not yet synchronized */
    return (int64_t)t * 1000;
}

static uint32_t uptime_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

/* ---- Serialized TX path -------------------------------------------------
 * NEVER send on the WebSocket from the WS client's OWN task: the DATA event
 * callback (on_ws -> fp_session_message_handle -> reply) runs inside that
 * task, and a large reply (e.g. a full dp_read snapshot > buffer_size) then
 * blocks in esp_websocket_client_send_text() while the client task is stuck
 * dispatching the event -> self-deadlock. All senders therefore enqueue the
 * frame here; the dedicated tx_task ships it from its own context. */

/* Takes ownership of the malloc'd frame; frees it if the queue is full. */
static bool tx_enqueue_owned(fp_ctx_t *c, char *json) {
    if (!json) return false;
    if (!c->txq || xQueueSend(c->txq, &json, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full — frame dropped");
        free(json);
        return false;
    }
    return true;
}

/* Successful transmissions since start. This is the PROGRESS anchor for the
 * watchdog heartbeat: found live 2026-07-09 — after abrupt server closes the
 * esp_websocket_client can wedge half-open WITHOUT a DISCONNECTED event, so
 * fp_session_running() stays true. A heartbeat tied to that STATE keeps the
 * WD_SESSION channel fed forever and the whole escalation goes blind; tying
 * it to actual send SUCCESS (below) starves the heartbeat in exactly that
 * case, so the watchdog restarts the client (which heals the wedge). */
static volatile uint32_t s_tx_ok_count;
static volatile uint32_t s_tx_fail_count;   /* dropped frames (link metric) */

void fountain_proto_tx_stats(uint32_t *ok, uint32_t *fail)
{
    if (ok)   *ok   = s_tx_ok_count;
    if (fail) *fail = s_tx_fail_count;
}

static void tx_task_fn(void *arg) {
    fp_ctx_t *c = (fp_ctx_t *)arg;
    char *json;
    for (;;) {
        if (xQueueReceive(c->txq, &json, portMAX_DELAY) != pdTRUE) continue;
        if (fp_ws_send(c->ws, json)) {
            s_tx_ok_count++;
        } else {
            s_tx_fail_count++;
            ESP_LOGW(TAG, "ws send failed (disconnected?) — frame dropped");
        }
        free(json);
    }
}

/* Transport adapter for fp_session: copy the frame (the session frees its
 * buffer right after this returns) and hand it to the TX queue of the
 * fp_ctx_t given as transport context — the adapter itself is instance-
 * agnostic (prerequisite for additional transports, firmware_server.md §16). */
static bool ws_send_adapter(void *ctx, const char *json) {
    return tx_enqueue_owned((fp_ctx_t *)ctx, strdup(json));
}

static void lock(void)   { xSemaphoreTake(s_ctx.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_ctx.lock); }

static void on_ws(fp_ws_event_t ev, const char *data, size_t len, void *user) {
    fp_ctx_t *c = (fp_ctx_t *)user;
    (void)len;
    lock();
    switch (ev) {
        case FP_WS_CONNECTED:
            fp_session_reset(&c->session);
            fp_session_hello_send(&c->session);
            break;
        case FP_WS_DATA:
            fp_session_message_handle(&c->session, data);
            break;
        case FP_WS_DISCONNECTED:
            fp_session_reset(&c->session);
            break;
    }
    unlock();
}

/* Connectivity supervision moved OUT of this task (work package 4): the
 * former two-stage watchdog (180 s -> WS restart, 5 fruitless restarts +
 * link -> reboot) now lives in the app_watchdog's WD_SESSION channel with
 * identical semantics. This task only REPORTS progress via cfg->on_alive
 * (once per second while the session is negotiated & running) and offers
 * fountain_proto_recover() as the stage-1 action (hard WS-client restart —
 * the esp_websocket_client was observed to wedge after aborted TLS). */
static void task_fn(void *arg) {
    fp_ctx_t *c = (fp_ctx_t *)arg;
    uint32_t last_hb = 0, last_rep = 0;
    uint32_t last_tx_ok = 0;
    bool was_running = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t up = uptime_s();

        if (!fp_session_running(&c->session)) {
            if (was_running && c->cfg->on_session_lost)
                c->cfg->on_session_lost(c->cfg->user);
            was_running = false;
            continue;
        }
        was_running = true;

        /* Progress = a frame actually LEFT the device since the last tick
         * (heartbeat/report guarantee one send per 60-s grid, well inside
         * the 180-s deadline). Session "running" alone is NOT progress —
         * see s_tx_ok_count above (half-open wedge). */
        uint32_t tx_ok = s_tx_ok_count;
        if (tx_ok != last_tx_ok) {
            last_tx_ok = tx_ok;
            if (c->cfg->on_alive) c->cfg->on_alive(c->cfg->user);
        }
        uint32_t hb_s  = s_slow_mode ? FP_SLOW_INTERVAL_S : c->cfg->heartbeat_s;
        uint32_t rep_s = s_slow_mode ? FP_SLOW_INTERVAL_S : c->cfg->report_s;
        lock();
        if (up - last_hb >= hb_s) {
            last_hb = up;
            fp_session_heartbeat_send(&c->session, up);
        }
        if (up - last_rep >= rep_s) {
            last_rep = up;
            fp_session_dp_report_send(&c->session, NULL);
        } else {
            /* Between full reports: push ONLY changed points (discrete /
             * past-deadband) so state flips reach the server within ~1 s.
             * No-op (nothing sent) while values are steady. */
            fp_session_dp_changes_send(&c->session);
        }
        unlock();
    }
}

bool fountain_proto_start(const fp_config_t *cfg) {
    if (!cfg || !cfg->auth_key || cfg->auth_key_len == 0) {
        ESP_LOGE(TAG, "invalid configuration");
        return false;
    }
    s_ctx.cfg = cfg;
    s_ctx.lock = xSemaphoreCreateMutex();
    if (!s_ctx.lock) return false;

    /* Transport context = this fp_ctx_t: the adapter enqueues into ITS
     * txq (instance-bound, not the file-global — firmware_server.md §16). */
    fp_session_init(&s_ctx.session, cfg, ws_send_adapter, &s_ctx);
    s_ctx.ws = fp_ws_create(cfg, on_ws, &s_ctx);
    if (!s_ctx.ws) { ESP_LOGE(TAG, "fp_ws_create failed"); return false; }

    /* Serialized TX path (queue + own sender task) BEFORE the WS starts,
     * so the very first hello already goes through it. */
    s_ctx.txq = xQueueCreate(FP_TXQ_DEPTH, sizeof(char *));
    if (!s_ctx.txq) return false;
    if (xTaskCreate(tx_task_fn, "fp_tx", 4096, &s_ctx, 5, &s_ctx.tx_task) != pdPASS) {
        ESP_LOGE(TAG, "tx task creation failed");
        return false;
    }

    if (!fp_ws_start(s_ctx.ws)) { ESP_LOGE(TAG, "fp_ws_start failed"); return false; }
    if (xTaskCreate(task_fn, "fp_task", 8192, &s_ctx, 5, &s_ctx.task) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return false;
    }
    ESP_LOGI(TAG, "started (device=%s)", cfg->device_id);
    return true;
}

bool fountain_proto_alert_send(const char *code, const char *severity,
                               const char *datapoint, double value,
                               double threshold, const char *detail) {
    if (!s_ctx.ws) return false;
    fp_device_alert_t a = {0};
    a.code = code; a.has_code = true;
    a.severity = severity; a.has_severity = true;
    if (datapoint) { a.datapoint = datapoint; a.has_datapoint = true;
                     a.value = (float)value; a.has_value = true;
                     a.threshold = (float)threshold; a.has_threshold = true; }
    if (detail) { a.detail = detail; a.has_detail = true; }

    cJSON *body = fp_device_alert_build(&a);
    cJSON *msg = fp_envelope_build(FP_MSG_DEVICE_ALERT, body, s_ctx.cfg->serial,
                                   NULL, NULL, fp_now_ms());
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return tx_enqueue_owned(&s_ctx, json); /* queue owns/frees the frame */
}

void fountain_proto_ota_failed_note(void) {
    bool fire = false;
    lock();
    fire = fp_session_ota_failed_note(&s_ctx.session);
    unlock();
    if (fire && s_ctx.cfg->on_ready) s_ctx.cfg->on_ready(s_ctx.cfg->user);
}

bool fountain_proto_ota_status_send(const char *target_version, const char *state,
                                    uint8_t attempt, uint8_t progress_pct,
                                    const char *error, const char *error_detail) {
    if (!s_ctx.ws) return false;
    fp_ota_status_t st = {0};
    if (target_version) { st.target_version = target_version; st.has_target_version = true; }
    if (state)          { st.state = state; st.has_state = true; }
    st.attempt = attempt; st.has_attempt = true;
    st.progress_pct = progress_pct; st.has_progress_pct = true;
    if (error)        { st.error = error; st.has_error = true; }
    if (error_detail) { st.error_detail = error_detail; st.has_error_detail = true; }

    cJSON *body = fp_ota_status_build(&st);
    cJSON *msg = fp_envelope_build(FP_MSG_OTA_STATUS, body, s_ctx.cfg->serial,
                                   NULL, NULL, fp_now_ms());
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return tx_enqueue_owned(&s_ctx, json); /* queue owns/frees the frame */
}
