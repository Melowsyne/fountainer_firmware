/* Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 * Host test mock: constant free heap. */
#pragma once
#include <stdint.h>

static inline uint32_t esp_get_free_heap_size(void) { return 120000u; }
static inline uint32_t esp_get_minimum_free_heap_size(void) { return 90000u; }
