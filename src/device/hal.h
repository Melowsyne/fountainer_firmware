/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"      /* GPIO_NUM_* */

/* =============================================================
 * hal module — Hardware Abstraction Layer.
 * Encapsulates direct hardware access: SSR/relay (pump) via LEDC PWM,
 * pressure sensor (0-5V) via external ADS1115 ADC on the I2C bus and the
 * internal temperature sensor of the S3.
 * (AM2302/DHT22 has its own module: onewire_am2302, uses HAL_DHT_GPIO.)
 *
 * Convention: all functions return bool (true = ok, false = error).
 * ============================================================= */

/* --- Pin/bus assignment of the connected devices ----------------------------
 * (taken from the verified predecessor project — wiring correct) */
#define HAL_DHT_GPIO            GPIO_NUM_4   /* AM2302/DHT22 data line (single-wire) */
#define HAL_I2C_SDA_GPIO        GPIO_NUM_8   /* I2C SDA -> ADS1115 (left header)     */
#define HAL_I2C_SCL_GPIO        GPIO_NUM_7   /* I2C SCL -> ADS1115 (left header)     */
#define HAL_SSR_GPIO            GPIO_NUM_5   /* Solid-State-Relay (pump), LEDC-PWM   */

/* Note: NOT "hal_init" — this name collides with a global symbol
 * in the ESP-IDF WiFi blob (libpp.a). Hence "hal_setup". */
bool hal_setup(void);

/* Relay */
bool hal_relay_set(bool bOn);
bool hal_relay_get(bool *pbOutOn);

/* Pressure sensor: raw ADC value or voltage at the pin in millivolts.
 * Note: 0-5V sensor requires a voltage divider to <=3.3V.
 * The mV -> bar conversion is done calibrated in hal_pressure_bar_read(). */
bool hal_pressure_mv_read(uint32_t *pulOutMv);
bool hal_pressure_bar_read(float *pflOutBar);

/* Combined read: mV AND bar from a single ADC conversion (avoids double
 * sampling when both the diagnostic voltage and the pressure are needed).
 * Either output pointer may be NULL. */
bool hal_pressure_read(uint32_t *pulOutMv, float *pflOutBar);

/* Runtime sensor curve (Fon_Sensor_Range_Bar/_Scale/_Offset datapoints):
 * bar = clamp((V-0.5)/4.0 * range) * scale + offset. Defaults keep the
 * original hardware value (500 PSI = 34.47 bar full scale). */
void hal_pressure_calibration_set(float flRangeBar, float flScale,
                                  int16_t sOffsetMbar);

/* Spread (max-min, sensor-mV) of the last 4-sample burst — early indicator
 * for wiring/grounding issues. Pump-task context only (no locking). */
uint32_t hal_pressure_noise_mv_get(void);

/* Internal temperature sensor of the ESP32-S3 */
bool hal_internal_temp_read(float *pflOutCelsius);

/* USB connection: true if a USB cable is plugged into the ESP32 (VBUS detected).
 * Used by the logging to produce local output only when a
 * console is attached. */
bool hal_usb_connected_get(bool *pbOutConnected);
