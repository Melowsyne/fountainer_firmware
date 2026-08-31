/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* fp_ws — esp_websocket_client wrapper (wss:// + CA pinning + Bearer header).
 * ESP-IDF-specific (not host-compilable). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "fountain_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FP_WS_CONNECTED, FP_WS_DATA, FP_WS_DISCONNECTED } fp_ws_event_t;

/* data/len valid only for FP_WS_DATA (fully reassembled text frame). */
typedef void (*fp_ws_event_cb)(fp_ws_event_t ev, const char *data, size_t len,
                               void *user);

typedef struct fp_ws_s fp_ws_t;

fp_ws_t *fp_ws_create(const fp_config_t *cfg, fp_ws_event_cb cb, void *user);
bool     fp_ws_start(fp_ws_t *ws);
bool     fp_ws_restart(fp_ws_t *ws);   /* watchdog: stop + start (recovers a wedged client) */
bool     fp_ws_send(fp_ws_t *ws, const char *json);   /* text frame */
void     fp_ws_destroy(fp_ws_t *ws);

#ifdef __cplusplus
}
#endif
