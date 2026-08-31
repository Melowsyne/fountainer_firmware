/* Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 * Host test mock: NVS as a no-op. get_blob returns NOT_FOUND -> the store
 * falls back to the compiled-in defaults at init. set/commit ok. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h)
{ (void)ns; (void)m; *h = 1; return ESP_OK; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *l)
{ (void)h; (void)k; (void)o; (void)l; return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t l)
{ (void)h; (void)k; (void)v; (void)l; return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void      nvs_close(nvs_handle_t h)  { (void)h; }
static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{ (void)h; (void)k; return ESP_OK; }
static inline esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *o)
{ (void)h; (void)k; (void)o; return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v)
{ (void)h; (void)k; (void)v; return ESP_OK; }
