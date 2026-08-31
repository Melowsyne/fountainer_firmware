/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "fp_ws.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "fp_ws";

struct fp_ws_s {
    esp_websocket_client_handle_t client;
    fp_ws_event_cb cb;
    void *user;
    /* Reassembly of fragmented text frames */
    char  *rx;
    size_t rx_len, rx_cap;
};

static void rx_reset(fp_ws_t *w) { w->rx_len = 0; if (w->rx) w->rx[0] = '\0'; }

static bool rx_append(fp_ws_t *w, const char *d, size_t n) {
    if (w->rx_len + n + 1 > w->rx_cap) {
        size_t nc = w->rx_cap ? w->rx_cap : 512;
        while (nc < w->rx_len + n + 1) nc *= 2;
        char *np = realloc(w->rx, nc);
        if (!np) return false;
        w->rx = np; w->rx_cap = nc;
    }
    memcpy(w->rx + w->rx_len, d, n);
    w->rx_len += n; w->rx[w->rx_len] = '\0';
    return true;
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)base;
    fp_ws_t *w = (fp_ws_t *)arg;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;

    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected");
            w->cb(FP_WS_CONNECTED, NULL, 0, w->user);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            rx_reset(w);
            w->cb(FP_WS_DISCONNECTED, NULL, 0, w->user);
            break;
        case WEBSOCKET_EVENT_DATA:
            if (e->op_code == 0x08) break;          /* ignore close frame */
            if (e->op_code != 0x01 && e->op_code != 0x00) break;  /* only text/cont */
            if (e->payload_offset == 0) rx_reset(w);
            if (e->data_len > 0) rx_append(w, e->data_ptr, e->data_len);
            if (e->payload_offset + e->data_len >= e->payload_len) {
                if (w->rx_len > 0)
                    w->cb(FP_WS_DATA, w->rx, w->rx_len, w->user);
                rx_reset(w);
            }
            break;
        default:
            break;
    }
}

fp_ws_t *fp_ws_create(const fp_config_t *cfg, fp_ws_event_cb cb, void *user) {
    fp_ws_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->cb = cb; w->user = user;

    /* Assemble the URI incl. device_id query. With CA -> wss:// (TLS, Trust
     * Anchor Pinning); without CA -> ws:// (plaintext, e.g. local test). */
    const char *scheme = cfg->ca_pem ? "wss" : "ws";
    char uri[256];
    snprintf(uri, sizeof uri, "%s://%s:%d%s?device_id=%s",
             scheme, cfg->server_host, cfg->server_port, cfg->server_path, cfg->device_id);
    char authz[256];
    snprintf(authz, sizeof authz, "Authorization: Bearer %s\r\n", cfg->bearer_token);

    esp_websocket_client_config_t wc = {0};
    wc.uri = uri;
    wc.cert_pem = cfg->ca_pem;        /* Trust Anchor Pinning */
    /* Mutual TLS: the device authenticates itself with its client
     * certificate; the server (ssl.CERT_REQUIRED) rejects anything else. */
    wc.client_cert = cfg->client_cert_pem;
    wc.client_key  = cfg->client_key_pem;
    wc.headers = authz;
    wc.subprotocol = "fountain";
    /* 10 s instead of 5 s: dampens reconnect storms on a lossy link (each
     * attempt is a full mTLS handshake worth of airtime). The IDF client
     * has no runtime setter for a true exponential backoff (config is
     * init-only) — deliberate deviation from Link_Robustness_v1 §B3; the
     * real storm brake is TLS session resumption + the 180-s WD grid. */
    wc.reconnect_timeout_ms = 10000;
    wc.network_timeout_ms = 10000;
    wc.buffer_size = 1024;
    /* The DATA callback runs in the WS client's task and does JSON parsing
     * + HMAC/SHA256 + canonicalization there — the default stack (~4 KB) is not enough
     * (stack overflow). Size it generously. */
    wc.task_stack = 8192;

    w->client = esp_websocket_client_init(&wc);
    if (!w->client) { free(w); return NULL; }
    esp_websocket_register_events(w->client, WEBSOCKET_EVENT_ANY, ws_event, w);
    return w;
}

bool fp_ws_start(fp_ws_t *w) {
    return w && esp_websocket_client_start(w->client) == ESP_OK;
}

/* Hard restart of the WS client (connectivity watchdog): tears the client
 * task down and brings it up again. Recovers a client whose internal
 * reconnect machinery wedged (observed after an aborted TLS session). */
bool fp_ws_restart(fp_ws_t *w) {
    if (!w) return false;
    rx_reset(w);
    esp_websocket_client_stop(w->client);    /* joins the client task */
    return esp_websocket_client_start(w->client) == ESP_OK;
}

bool fp_ws_send(fp_ws_t *w, const char *json) {
    if (!w || !esp_websocket_client_is_connected(w->client)) return false;
    int len = (int)strlen(json);
    /* Bounded timeout instead of portMAX_DELAY: if the TX path wedges, the
     * sender drops the frame and stays alive instead of hanging forever
     * (defense in depth on top of the serialized TX task in fp_task). */
    return esp_websocket_client_send_text(w->client, json, len,
                                          pdMS_TO_TICKS(10000)) == len;
}

void fp_ws_destroy(fp_ws_t *w) {
    if (!w) return;
    if (w->client) {
        esp_websocket_client_stop(w->client);
        esp_websocket_client_destroy(w->client);
    }
    free(w->rx);
    free(w);
}
