/*
 * Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 *
 * This source code is proprietary. No license is granted to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of
 * this software without prior written permission from the copyright holder.
 *
 * For licensing inquiries, contact: info@melowsyne.com
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
 * Resolved at RUNTIME from the factory hw_revision (factory_config), so the
 * one release image runs on every hardware (serienfertigung.md §2):
 *   HW1.x  ESP32-S3-DevKitC testbed (verified predecessor wiring)
 *   HW2.x  fountainer_hw_24v PCB Rev. 1 (status LEDs, CAN, CP2102N on UART0)
 * Devices without factory partition resolve to HW1.0 (legacy identity). */
typedef struct {
    gpio_num_t eDht;        /* AM2302/DHT22 data (HW2.x: n/c dummy pin — no
                             * DHT on that board; reads fail gracefully)     */
    gpio_num_t eI2cSda;     /* I2C -> ADS1115                                */
    gpio_num_t eI2cScl;
    gpio_num_t eSsr;        /* Solid-State-Relay (pump), LEDC-PWM            */
    gpio_num_t eLedRed;     /* status LEDs, ACTIVE LOW (anode via R at 3V3); */
    gpio_num_t eLedGreen;   /* GPIO_NUM_NC on boards without them            */
    float      flSensorVdiv;/* V_adc / V_sensor at the pressure input: 1.0 on
                             * the devkit (direct); 0.6 on the 24V PCB
                             * (R17/R20 = 10k/15k before ADS1115 at 3V3)    */
    gpio_num_t eCanTx;      /* TWAI TX -> TJA1051T/3 TXD; GPIO_NUM_NC on   */
    gpio_num_t eCanRx;      /* boards without transceiver (devkit)         */
    gpio_num_t eCanStby;    /* TJA1051T/3 S pin (net CAN_PWDN): HIGH =
                             * silent/standby (boot default), LOW = normal  */
} hal_pins_t;

/* Lazily resolved on first use; factory_config_init() has run by then in the
 * main_init order. Before factory init it would resolve to HW1.0 — exactly
 * the legacy behaviour, so no init-order hazard. */
const hal_pins_t *hal_pins(void);

/* Call-site compatibility: the historical single-pin macros now read the
 * runtime map. */
#define HAL_DHT_GPIO            (hal_pins()->eDht)
#define HAL_I2C_SDA_GPIO        (hal_pins()->eI2cSda)
#define HAL_I2C_SCL_GPIO        (hal_pins()->eI2cScl)
#define HAL_SSR_GPIO            (hal_pins()->eSsr)

/* Note: NOT "hal_init" — this name collides with a global symbol
 * in the ESP-IDF WiFi blob (libpp.a). Hence "hal_setup". */
bool hal_setup(void);

/* CAN transceiver standby (S pin). hal_setup() parks it in standby; the
 * CANopen task releases it once the TWAI driver runs. No-op without pin. */
void hal_can_standby_set(bool bStandby);
bool hal_can_available(void);

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
