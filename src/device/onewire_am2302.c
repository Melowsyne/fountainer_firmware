/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "onewire_am2302.h"
#include "hal.h"          /* HAL_DHT_GPIO */
#include "debug.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define TAG "am2302"

/* Protects the timing-critical bit-banging from scheduler/IRQ interruption. */
static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

bool am2302_init(gpio_num_t ePin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << ePin),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,   /* Open-Drain: 0=LOW, 1=Hi-Z */
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) return false;
    gpio_set_level(ePin, 1);                         /* Idle HIGH */
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "init (GPIO %d)", (int)ePin);
    return true;
}

/* Failure diagnosis of the last dht_read_raw (wiring debug): which handshake
 * phase timed out, and what the line idled at BEFORE the start pulse.
 *   phase 1: sensor never pulled LOW after start (no response at all)
 *   phase 2: response LOW never ended        (line stuck LOW)
 *   phase 3: response HIGH never ended       (preamble stuck HIGH)
 *   phase 4: bit LOW/HIGH timing broke mid-stream
 * idle 0 with a healthy pull-up means short / missing pull-up / sensor
 * holding the bus; idle 1 + phase 1 means the sensor simply doesn't answer
 * (power, wrong pin, dead sensor). */
static uint8_t  s_ucLastFailPhase;
static uint8_t  s_ucLastIdleLevel;
/* Phase-2 post-mortem: extra time (us) the line stayed LOW beyond the
 * timeout, capped at 20 ms; 20000 = never rose. ~0-200 us points at a weak
 * pull-up / cable RC, milliseconds at a capacitor (data wire on the sensor's
 * VCC pin, or an unpowered sensor feeding itself through the data line). */
static uint32_t s_ulLastLowExtraUs;

/* Reads 40 raw bits (5 bytes) from the AM2302. true = ok, false = timeout. */
static bool dht_read_raw(gpio_num_t ePin, uint8_t data[5])
{
    s_ucLastIdleLevel = (uint8_t)gpio_get_level(ePin);

    /* Start pulse: >=1 ms LOW. esp_rom_delay_us instead of vTaskDelay, because
     * pdMS_TO_TICKS(2) would yield 0 ticks at a 100 Hz tick rate. */
    gpio_set_level(ePin, 0);
    esp_rom_delay_us(2000);

    portENTER_CRITICAL(&s_dht_mux);

    gpio_set_level(ePin, 1);                 /* release, pull-up -> HIGH */

    int t = 0;
    while (gpio_get_level(ePin)) {            /* sensor pulls LOW after 20-40 us */
        if (++t > 200) { portEXIT_CRITICAL(&s_dht_mux); s_ucLastFailPhase = 1; return false; }
        esp_rom_delay_us(1);
    }
    t = 0;
    while (!gpio_get_level(ePin)) {           /* LOW->HIGH (~80 us) */
        if (++t > 300) {
            portEXIT_CRITICAL(&s_dht_mux);
            s_ucLastFailPhase = 2;
            /* Post-mortem OUTSIDE the critical section: how much longer
             * does the line stay LOW? (wiring diagnosis, see above) */
            uint32_t ulExtra = 0;
            while (!gpio_get_level(ePin) && ulExtra < 20000) {
                esp_rom_delay_us(10);
                ulExtra += 10;
            }
            s_ulLastLowExtraUs = ulExtra;
            return false;
        }
        esp_rom_delay_us(1);
    }
    t = 0;
    while (gpio_get_level(ePin)) {            /* HIGH->LOW, data start */
        if (++t > 300) { portEXIT_CRITICAL(&s_dht_mux); s_ucLastFailPhase = 3; return false; }
        esp_rom_delay_us(1);
    }

    memset(data, 0, 5);
    for (int i = 0; i < 40; i++) {
        t = 0;
        while (!gpio_get_level(ePin)) {       /* end of the ~50 us LOW phase */
            if (++t > 200) { portEXIT_CRITICAL(&s_dht_mux); s_ucLastFailPhase = 4; return false; }
            esp_rom_delay_us(1);
        }
        /* Sample after 40 us: HIGH -> bit 1 (70 us), already LOW -> bit 0 (28 us). */
        esp_rom_delay_us(40);
        if (gpio_get_level(ePin)) {
            data[i / 8] |= (uint8_t)(1 << (7 - (i % 8)));
            t = 0;
            while (gpio_get_level(ePin)) {     /* wait out the rest of the HIGH phase */
                if (++t > 200) { portEXIT_CRITICAL(&s_dht_mux); s_ucLastFailPhase = 4; return false; }
                esp_rom_delay_us(1);
            }
        }
    }

    portEXIT_CRITICAL(&s_dht_mux);
    s_ucLastFailPhase = 0;
    return true;
}

uint8_t  am2302_last_fail_phase_get(void)   { return s_ucLastFailPhase; }
uint8_t  am2302_last_idle_level_get(void)   { return s_ucLastIdleLevel; }
uint32_t am2302_last_low_extra_us_get(void) { return s_ulLastLowExtraUs; }

bool am2302_climate_read(gpio_num_t ePin,
                         float *pflOutTemperatureC, float *pflOutHumidityPct)
{
    if (!pflOutTemperatureC || !pflOutHumidityPct) return false;

    uint8_t data[5];
    if (!dht_read_raw(ePin, data)) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "read error (timeout, GPIO %d, phase %u, idle %u, extra-low %u us)",
                (int)ePin, (unsigned)s_ucLastFailPhase,
                (unsigned)s_ucLastIdleLevel, (unsigned)s_ulLastLowExtraUs);
        return false;
    }

    uint8_t ucChecksum = data[0] + data[1] + data[2] + data[3];
    if (data[4] != ucChecksum) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "checksum error (expected 0x%02X, received 0x%02X)", ucChecksum, data[4]);
        return false;
    }

    *pflOutHumidityPct = (float)((data[0] << 8) | data[1]) * 0.1f;

    int16_t slRawTemp = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
    *pflOutTemperatureC = (float)slRawTemp * 0.1f;
    if (data[2] & 0x80) *pflOutTemperatureC = -*pflOutTemperatureC;

    logging(LOG_TARGET_AUTO, DBG_LVL_VERBOSE, TAG, "read %.1fC %.1f%%",
            *pflOutTemperatureC, *pflOutHumidityPct);
    return true;
}
