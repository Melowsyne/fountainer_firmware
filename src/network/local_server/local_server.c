/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * local_server — local WSS server, transport layer (firmware_server.md,
 * implementation plan, work package AP4). Design rules:
 *   - The WS handler (httpd task) stays short (§22): size check,
 *     rate limit, pool buffer, descriptor queue — never block (§23).
 *   - Payload lives in the fixed pool, queues carry only descriptors (§24).
 *   - Slots via counting semaphore (§27); lru_purge is OFF so that httpd
 *     does not bypass the semaphore and sacrifice active sessions for new clients.
 *   - Sending happens exclusively via httpd_queue_work in the httpd task
 *     (one TX owner per socket, §41).
 *   - A client error closes its SESSION, never the device (§42).
 */
#include "local_server.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_https_server.h"
#include "esp_timer.h"

#include "debug.h"

#include "event_manager.h"
#include "system_events.h"
#include "local_buffer_pool.h"
#include "task_com.h"
#include "wlan_com.h"

static const char *TAG = "local_srv";

/* ----- State ------------------------------------------------------------- */
typedef enum { DESC_OPEN, DESC_FRAME, DESC_CLOSE } desc_kind_t;

typedef struct {
    uint8_t        kind;          /* desc_kind_t */
    uint8_t        slot;
    uint32_t       conn_id;
    local_frame_t *frame;         /* DESC_FRAME only */
} local_desc_t;

static httpd_handle_t       s_hd;
static local_session_t      s_astSessions[LOCAL_SERVER_MAX_CLIENTS];
static SemaphoreHandle_t    s_hSlots;     /* counting: free sessions        */
static SemaphoreHandle_t    s_hLock;      /* session table                  */
static QueueHandle_t        s_hRxq;
static TaskHandle_t         s_hWorker;
static local_server_stats_t s_stStats;
static uint32_t             s_ulConnCtr;

#include "local_protocol.h"   /* Fountain processing (AP5) */

/* ----- TX: sole send path (httpd task via queue_work) -------------------- */
typedef struct {
    int    sockfd;
    size_t len;
    char   data[];
} tx_job_t;

static void tx_work(void *arg)
{
    tx_job_t *job = arg;
    httpd_ws_frame_t f = {
        .final = true, .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)job->data, .len = job->len,
    };
    /* We already run in the httpd task: only the direct sender is allowed.
     * httpd_ws_send_data() would queue another work item here and
     * wait for it with portMAX_DELAY -> deadlock of the httpd task. */
    if (s_hd) {
        esp_err_t e = httpd_ws_send_frame_async(s_hd, job->sockfd, &f);
        if (e != ESP_OK)
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "ws send failed (fd %d): %s",
                    job->sockfd, esp_err_to_name(e));
    }
    free(job);
}

bool local_server_send(uint8_t slot, uint32_t conn_id, const char *json)
{
    if (!s_hd || slot >= LOCAL_SERVER_MAX_CLIENTS) return false;
    local_session_t *s = &s_astSessions[slot];
    if (!s->active || s->connection_id != conn_id) return false;

    const size_t len = strlen(json);
    tx_job_t *job = malloc(sizeof *job + len);
    if (!job) return false;
    job->sockfd = s->sockfd;
    job->len = len;
    memcpy(job->data, json, len);
    if (httpd_queue_work(s_hd, tx_work, job) != ESP_OK) {
        free(job);
        return false;
    }
    return true;
}

/* ----- Session management ------------------------------------------------ */
static void session_cleanup(uint8_t ucSlot)
{
    local_session_t *s = &s_astSessions[ucSlot];
    if (!s->active) return;
    local_protocol_on_close(s);
    fp_session_close(&s->protocol);      /* wipe nonces/seq (§28) */
    xSemaphoreTake(s_hLock, portMAX_DELAY);
    s->active = false;
    s->state = LOCAL_SESSION_FREE;
    s->sockfd = -1;
    xSemaphoreGive(s_hLock);
    xSemaphoreGive(s_hSlots);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "session %u closed",
            (unsigned)ucSlot);
}

static bool desc_enqueue(uint8_t kind, uint8_t slot, uint32_t conn_id,
                         local_frame_t *frame)
{
    local_desc_t d = { .kind = kind, .slot = slot,
                       .conn_id = conn_id, .frame = frame };
    if (xQueueSend(s_hRxq, &d, 0) != pdTRUE) {          /* NEVER block (§23) */
        if (frame) local_buffer_release(frame);
        s_stStats.queue_overflows++;
        return false;
    }
    return true;
}

/* httpd calls this at socket end (client close, error, server stop). */
static void on_sess_free(void *ctx)
{
    local_session_t *s = ctx;
    if (!s || !s->active) return;
    s->state = LOCAL_SESSION_CLOSING;
    if (!desc_enqueue(DESC_CLOSE, (uint8_t)(s - s_astSessions),
                      s->connection_id, NULL))
        session_cleanup((uint8_t)(s - s_astSessions));   /* queue full: inline */
}

/* ----- WS handler (runs in the httpd task — keep it short!) -------------- */
static esp_err_t ws_handler(httpd_req_t *req)
{
    /* CAUTION IDF pin: in IDF 5.5.3 httpd calls the handler after the
     * WS handshake with method==HTTP_GET (httpd_uri.c). IDF 6 no longer
     * does that (return before uri->handler; instead
     * CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT) — this branch would never
     * run there: no slot, no sess_ctx. Adapt on the IDF upgrade! */
    if (req->method == HTTP_GET) {                       /* handshake done   */
        if (xSemaphoreTake(s_hSlots, 0) != pdTRUE) {
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "connection rejected: no free slot");
            s_stStats.forced_disconnects++;
            return ESP_FAIL;                             /* closes the socket */
        }
        local_session_t *s = NULL;
        xSemaphoreTake(s_hLock, portMAX_DELAY);
        for (int i = 0; i < LOCAL_SERVER_MAX_CLIENTS; i++) {
            if (!s_astSessions[i].active) { s = &s_astSessions[i]; break; }
        }
        if (s) {
            memset(s, 0, offsetof(local_session_t, protocol));
            s->active = true;
            s->connection_id = ++s_ulConnCtr;
            s->sockfd = httpd_req_to_sockfd(req);
            s->state = LOCAL_SESSION_CONNECTED;
            s->connected_us = s->last_activity_us = esp_timer_get_time();
            local_rate_limit_reset(&s->rate);
        }
        xSemaphoreGive(s_hLock);
        if (!s) { xSemaphoreGive(s_hSlots); return ESP_FAIL; }

        req->sess_ctx = s;
        req->free_ctx = on_sess_free;
        s_stStats.connections_total++;
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "client connected (slot %u, fd %d)",
                (unsigned)(s - s_astSessions), s->sockfd);
        desc_enqueue(DESC_OPEN, (uint8_t)(s - s_astSessions),
                     s->connection_id, NULL);
        return ESP_OK;
    }

    local_session_t *s = req->sess_ctx;
    if (!s || !s->active || s->state == LOCAL_SESSION_CLOSING) return ESP_FAIL;
    const uint8_t ucSlot = (uint8_t)(s - s_astSessions);

    httpd_ws_frame_t pkt = { 0 };
    if (httpd_ws_recv_frame(req, &pkt, 0) != ESP_OK) return ESP_FAIL;

    /* Size limit BEFORE any processing (§26); >= because of the NUL for cJSON. */
    if (pkt.len >= LOCAL_MAX_FRAME_SIZE) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "frame too large (%u) — forced close", (unsigned)pkt.len);
        s_stStats.forced_disconnects++;
        return ESP_FAIL;
    }
    /* Drain buffer for discarded frames: ONE suffices, the handler
     * runs exclusively in the httpd task (RAM is the bottleneck, M1). */
    static uint8_t s_aucDrain[LOCAL_MAX_FRAME_SIZE];

    if (pkt.type != HTTPD_WS_TYPE_TEXT) {                 /* text only (§50) */
        /* The payload MUST be drained from the stream — otherwise the
         * frame bytes stay unread, the next WS header is parsed wrongly
         * and the connection dies. (Found by fuzzing: a binary frame
         * killed the session.) Control frames (ping/pong/close) are handled
         * by httpd itself; what lands here is mainly BINARY data frames. */
        if (pkt.len > 0) {
            pkt.payload = s_aucDrain;
            httpd_ws_recv_frame(req, &pkt, sizeof s_aucDrain);
        }
        return ESP_OK;
    }

    /* Rate limit: a drop is cheap, repeated abuse closes the session (§33). */
    if (!local_rate_allow(&s->rate, 1.0f)) {
        s_stStats.rate_drops++;
        const bool bClose = local_rate_violation_note(&s->rate);
        pkt.payload = s_aucDrain;
        httpd_ws_recv_frame(req, &pkt, sizeof s_aucDrain);
        if (bClose) {
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "rate strikes exhausted — forced close (slot %u)", ucSlot);
            s_stStats.forced_disconnects++;
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    local_frame_t *frame = local_buffer_acquire();
    if (!frame) {                                         /* overload (§25) */
        s_stStats.queue_overflows++;
        pkt.payload = s_aucDrain;
        httpd_ws_recv_frame(req, &pkt, sizeof s_aucDrain);
        return ESP_OK;
    }
    pkt.payload = frame->data;
    if (httpd_ws_recv_frame(req, &pkt, LOCAL_MAX_FRAME_SIZE) != ESP_OK) {
        local_buffer_release(frame);
        return ESP_FAIL;
    }
    frame->data[pkt.len] = '\0';
    frame->length = (uint16_t)pkt.len;
    s->last_activity_us = esp_timer_get_time();
    s_stStats.frames_rx++;
    desc_enqueue(DESC_FRAME, ucSlot, s->connection_id, frame);
    return ESP_OK;
}

/* ----- Worker (ONE shared task for all slots, prio 3) -------------------- */
static void timeout_scan(void)
{
    const int64_t llNow = esp_timer_get_time();
    for (int i = 0; i < LOCAL_SERVER_MAX_CLIENTS; i++) {
        local_session_t *s = &s_astSessions[i];
        if (s->active && s->state != LOCAL_SESSION_CLOSING && s_hd &&
            llNow - s->last_activity_us > LOCAL_IDLE_TIMEOUT_US) {
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "session %d idle timeout — closing", i);
            s_stStats.forced_disconnects++;
            httpd_sess_trigger_close(s_hd, s->sockfd);   /* -> on_sess_free */
        }
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    local_desc_t d;
    for (;;) {
        if (xQueueReceive(s_hRxq, &d, pdMS_TO_TICKS(1000)) != pdTRUE) {
            timeout_scan();                              /* 1-s tick (§43) */
            continue;
        }
        local_session_t *s = &s_astSessions[d.slot];
        const bool bStale = !s->active || s->connection_id != d.conn_id;

        switch ((desc_kind_t)d.kind) {
        case DESC_OPEN:
            if (!bStale) local_protocol_on_open(s);
            break;
        case DESC_FRAME:
            if (!bStale && s->state != LOCAL_SESSION_CLOSING)
                local_protocol_on_frame(s, (const char *)d.frame->data);
            local_buffer_release(d.frame);
            break;
        case DESC_CLOSE:
            if (!bStale) session_cleanup(d.slot);
            break;
        }
    }
}

/* ----- Start/stop on the WLAN lifecycle ---------------------------------- */
static const httpd_uri_t s_stWsUri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
    .supported_subprotocol = "fountain",
};

static void server_start(void)
{
    if (s_hd) return;
    const char *cert = task_com_tls_client_cert_get();   /* combo cert (AP1) */
    const char *key  = task_com_tls_client_key_get();
    const char *ca   = task_com_tls_ca_get();
    if (!cert || !key || !ca) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "no TLS material — local server stays OFF (mTLS mandatory)");
        return;
    }

    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert     = (const uint8_t *)cert;
    cfg.servercert_len = strlen(cert) + 1;
    cfg.prvtkey_pem    = (const uint8_t *)key;
    cfg.prvtkey_len    = strlen(key) + 1;
    cfg.cacert_pem     = (const uint8_t *)ca;            /* => mTLS REQUIRED */
    cfg.cacert_len     = strlen(ca) + 1;
    cfg.port_secure    = LOCAL_SERVER_PORT;
    cfg.session_tickets = false;
    /* httpd socket capacity is NOT the session policy: the counting
     * semaphore (s_hSlots) enforces LOCAL_SERVER_MAX_CLIENTS Fountain sessions.
     * httpd however needs AT LEAST 2 sockets to work reliably
     * (with max_open_sockets=1 esp_https_server does not even accept the FIRST
     * connection cleanly). The one extra socket also allows a client beyond
     * the limit to be rejected CLEANLY via the semaphore
     * (ESP_FAIL in the GET handler) instead of leaving it hanging at TCP level. */
    cfg.httpd.max_open_sockets = LOCAL_SERVER_MAX_CLIENTS < 2
                                     ? 2 : LOCAL_SERVER_MAX_CLIENTS;
    cfg.httpd.lru_purge_enable = false;   /* slots governed by semaphore (§27) */
    cfg.httpd.task_priority    = 4;       /* below pump (6) and cloud (5)     */
    cfg.httpd.core_id          = tskNO_AFFINITY;
    cfg.httpd.ctrl_port        = 32769;

    esp_err_t e = httpd_ssl_start(&s_hd, &cfg);
    if (e != ESP_OK) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "httpd_ssl_start: %s", esp_err_to_name(e));
        s_hd = NULL;
        return;
    }
    httpd_register_uri_handler(s_hd, &s_stWsUri);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "wss://0.0.0.0:%d/ws (mTLS, max %d clients)",
            LOCAL_SERVER_PORT, LOCAL_SERVER_MAX_CLIENTS);
}

static void server_stop(void)
{
    if (!s_hd) return;
    httpd_handle_t hd = s_hd;
    s_hd = NULL;                     /* no more new TX jobs      */
    httpd_ssl_stop(hd);              /* closes sockets -> on_sess_free    */
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "stopped");
}

static void on_wlan_event(system_event_t eEvent, const void *pvData, size_t szSize)
{
    (void)pvData; (void)szSize;
    if (eEvent == EVT_WLAN_CONNECTED)         server_start();
    else if (eEvent == EVT_WLAN_DISCONNECTED) server_stop();
}

/* ----- API --------------------------------------------------------------- */
esp_err_t local_server_init(void)
{
    s_hLock = xSemaphoreCreateMutex();
    s_hSlots = xSemaphoreCreateCounting(LOCAL_SERVER_MAX_CLIENTS,
                                        LOCAL_SERVER_MAX_CLIENTS);
    s_hRxq = xQueueCreate(LOCAL_RX_QUEUE_DEPTH, sizeof(local_desc_t));
    if (!s_hLock || !s_hSlots || !s_hRxq) return ESP_ERR_NO_MEM;
    local_buffer_pool_init();

    if (xTaskCreate(worker_task, "local_worker", 6144, NULL, 3,
                    &s_hWorker) != pdPASS)
        return ESP_FAIL;

    event_manager_subscribe(EVT_WLAN_CONNECTED, on_wlan_event);
    event_manager_subscribe(EVT_WLAN_DISCONNECTED, on_wlan_event);
    if (wlan_com_connected_get()) server_start();
    return ESP_OK;
}

bool local_server_running(void) { return s_hd != NULL; }

bool local_server_session_send(local_session_t *s, const char *json)
{
    return local_server_send((uint8_t)(s - s_astSessions),
                             s->connection_id, json);
}

void local_server_session_close(local_session_t *s, local_close_reason_t eReason)
{
    if (!s_hd || !s->active || s->state == LOCAL_SESSION_CLOSING) return;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "session %u forced close (reason %d)",
            (unsigned)(s - s_astSessions), (int)eReason);
    s_stStats.forced_disconnects++;
    httpd_sess_trigger_close(s_hd, s->sockfd);           /* -> on_sess_free */
}

void local_server_stats_get(local_server_stats_t *out) { *out = s_stStats; }

uint8_t local_server_client_count(void)
{
    uint8_t ucN = 0;
    for (int i = 0; i < LOCAL_SERVER_MAX_CLIENTS; i++)
        if (s_astSessions[i].active) ucN++;
    return ucN;
}
