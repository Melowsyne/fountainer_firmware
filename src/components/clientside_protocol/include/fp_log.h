/*
 * Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 *
 * This source code is proprietary. No license is granted to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of
 * this software without prior written permission from the copyright holder.
 *
 * For licensing inquiries, contact: info@melowsyne.com
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
