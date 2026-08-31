/* Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 * Host test mock: heap_caps as constant values. */
#pragma once
#include <stddef.h>

#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_8BIT    (1 << 2)

static inline size_t heap_caps_get_largest_free_block(unsigned caps)
{ (void)caps; return 80000u; }
