/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

/* =============================================================
 * onewire_am2302 — AM2302/DHT22 driver (single-wire protocol,
 * bit-banged in a short critical section — timing-critical).
 *
 * The pin is a parameter: the DHT protocol has NO addressing,
 * so every sensor needs its OWN data pin (currently one sensor
 * on HAL_DHT_GPIO; more can be added without driver changes).
 * Per-sensor minimum interval between reads: 2 s.
 * ============================================================= */

bool am2302_init(gpio_num_t ePin);

/* Reads temperature (°C) and relative humidity (%) from the sensor on
 * ePin. */
bool am2302_climate_read(gpio_num_t ePin,
                         float *pflOutTemperatureC, float *pflOutHumidityPct);

/* Wiring diagnosis of the LAST failed read (see dht_read_raw phases):
 * 0=ok, 1=no response, 2=stuck LOW, 3=preamble stuck HIGH, 4=bit timing. */
uint8_t am2302_last_fail_phase_get(void);
/* Line level right before the start pulse (1 = pull-up ok, 0 = stuck LOW). */
uint8_t am2302_last_idle_level_get(void);
/* Phase-2 only: extra us the line stayed LOW past the timeout (cap 20000). */
uint32_t am2302_last_low_extra_us_get(void);
