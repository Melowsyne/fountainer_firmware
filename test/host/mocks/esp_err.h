/* Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 * Host test mock: esp_err_t + NVS error codes. */
#pragma once

typedef int esp_err_t;

#define ESP_OK                        0
#define ESP_FAIL                     -1
#define ESP_ERR_NVS_NOT_FOUND         0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES     0x1103
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1104

#define ESP_ERROR_CHECK(x) ((void)(x))

static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "esp_err"; }
