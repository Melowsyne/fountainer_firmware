/* Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 * Host test mock: minimal FreeRTOS types for datapoints.c (no RTOS). */
#pragma once
#include <stdint.h>
#include <assert.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;

#define portMAX_DELAY   0xffffffffUL
#define pdTRUE          1
#define pdFALSE         0

#define configASSERT(x) assert(x)
