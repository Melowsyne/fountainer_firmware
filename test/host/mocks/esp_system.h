/* Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 * Host-Test-Mock: konstanter freier Heap. */
#pragma once
#include <stdint.h>

static inline uint32_t esp_get_free_heap_size(void) { return 120000u; }
static inline uint32_t esp_get_minimum_free_heap_size(void) { return 90000u; }
