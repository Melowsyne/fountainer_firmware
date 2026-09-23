/* Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 * Host-Test-Mock: NVS-Flash-Init als No-op. */
#pragma once
#include "esp_err.h"

static inline esp_err_t nvs_flash_init(void)  { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }
