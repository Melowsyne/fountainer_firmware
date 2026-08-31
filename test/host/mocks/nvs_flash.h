/* Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 * Host test mock: NVS flash init as a no-op. */
#pragma once
#include "esp_err.h"

static inline esp_err_t nvs_flash_init(void)  { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }
