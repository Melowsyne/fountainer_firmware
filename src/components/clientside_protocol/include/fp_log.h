/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* fp_log — thin log shim: ESP_LOGx on the target, printf on the host.
 * Keeps fp_session/fp_envelope/fp_auth host-compilable. */
#pragma once

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define FP_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define FP_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define FP_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define FP_LOGI(tag, fmt, ...) fprintf(stdout, "[I %s] " fmt "\n", tag, ##__VA_ARGS__)
#define FP_LOGW(tag, fmt, ...) fprintf(stdout, "[W %s] " fmt "\n", tag, ##__VA_ARGS__)
#define FP_LOGE(tag, fmt, ...) fprintf(stderr, "[E %s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
