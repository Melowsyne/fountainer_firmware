/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "hal.h"
#include "debug.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "soc/gpio_reg.h"

#define TAG "hal"

/* --- I2C bus / ADS1115 (12/16-bit ADC for the 0-5V pressure sensor) ---------- */
#define HAL_I2C_PORT       I2C_NUM_0
#define ADS1115_ADDR       0x48      /* ADDR -> GND                              */
#define ADS1115_REG_CONV   0x00
#define ADS1115_REG_CFG    0x01
/* Single-Shot, AIN0-GND, PGA = +-4,096 V, 250 SPS, comparator disabled.
 * Rate choice from the noise measurement series (plan 5.2): 860 SPS
 * (1.2-ms integration) raised the per-sample noise enough to eat most of
 * the trimming gain (burst spread up to 5 mV); 250 SPS (DR=101, ~4-ms
 * integration) keeps the ADC's intrinsic filtering AND allows a 4-sample
 * burst in ~18 ms per 200-ms control cycle. */
#define ADS1115_CFG_HI     0xC3
#define ADS1115_CFG_LO     0xA3
/* Burst parameters (consolidation plan 5.2): 4 conversions, drop min+max,
 * average the middle two — suppresses single spikes BEFORE the EMA. */
#define ADS1115_BURST_N    4
#define ADS1115_POLL_US    500       /* conversion-ready poll step           */
#define ADS1115_POLL_MAX   14        /* ~7 ms upper bound per conversion     */
#define ADS1115_FSR_V      4.096f    /* Full-scale range at PGA +-4,096 V        */
#define ADS1115_RES        32768.0f  /* 2^15 (signed)                            */
/* No active voltage divider — ADS1115 runs on 5V, sensor max 4,5V. */
#define HAL_VDIV_RATIO     1.0f

/* Pressure sensor curve: 0,5 V -> 0 PSI, 4,5 V -> 500 PSI. The skeleton
 * pipeline (data_store, fountain_controlling) calculates in bar -> conversion. */
#define PRESSURE_V_MIN     0.5f
#define PRESSURE_V_MAX     4.5f
#define PRESSURE_MAX_PSI   500.0f
#define PSI_TO_BAR         0.0689476f

/* --- LEDC / SSR (pump) ------------------------------------------------------- */
#define HAL_LEDC_TIMER     LEDC_TIMER_0
#define HAL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define HAL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define HAL_LEDC_DUTY_BITS LEDC_TIMER_10_BIT       /* 0..1023                     */
#define HAL_LEDC_DUTY_MAX  ((1u << HAL_LEDC_DUTY_BITS) - 1u)
#define HAL_LEDC_FREQ_HZ   1000

/* --- Internal handles ------------------------------------------------------- */
static i2c_master_bus_handle_t     s_i2c_bus  = NULL;
static i2c_master_dev_handle_t     s_ads1115  = NULL;
static temperature_sensor_handle_t s_tsens    = NULL;
static bool                        s_relay_on  = false;

/* ============================================================================
 * ADS1115
 * ========================================================================== */
/* One single-shot conversion @860 SPS: start, poll the OS (conversion
 * ready) bit instead of a fixed delay (the 100-Hz FreeRTOS tick is far too
 * coarse for 1.2-ms conversions -> short us-polling), read the result. */
static esp_err_t ads1115_convert_once(int16_t *pslRaw)
{
    uint8_t cfg[3] = { ADS1115_REG_CFG, ADS1115_CFG_HI, ADS1115_CFG_LO };
    esp_err_t e = i2c_master_transmit(s_ads1115, cfg, sizeof(cfg), 100);
    if (e != ESP_OK) return e;

    uint8_t reg = ADS1115_REG_CFG;
    uint8_t buf[2] = { 0 };
    bool bReady = false;
    for (int i = 0; i < ADS1115_POLL_MAX; i++) {
        esp_rom_delay_us(ADS1115_POLL_US);
        e = i2c_master_transmit_receive(s_ads1115, &reg, 1, buf, 2, 100);
        if (e != ESP_OK) return e;
        if (buf[0] & 0x80) { bReady = true; break; }   /* OS=1: idle */
    }
    if (!bReady) return ESP_ERR_TIMEOUT;

    reg = ADS1115_REG_CONV;
    e = i2c_master_transmit_receive(s_ads1115, &reg, 1, buf, 2, 100);
    if (e != ESP_OK) return e;

    *pslRaw = (int16_t)((buf[0] << 8) | buf[1]);
    return ESP_OK;
}

/* Burst noise figure of the last successful burst (max-min, in mV at the
 * sensor) — read by the pump task only (same context as the read itself). */
static uint32_t s_ulBurstNoiseMv;
uint32_t hal_pressure_noise_mv_get(void) { return s_ulBurstNoiseMv; }

/* Burst read with outlier trimming: ADS1115_BURST_N conversions; with all
 * four good, min+max are dropped and the middle two averaged. Degrades
 * gracefully: >=2 good samples -> plain average; fewer -> error. */
static esp_err_t ads1115_read_raw(int16_t *pslRaw)
{
    int32_t aslS[ADS1115_BURST_N];
    int slN = 0;
    for (int i = 0; i < ADS1115_BURST_N; i++) {
        int16_t slV = 0;
        if (ads1115_convert_once(&slV) == ESP_OK)
            aslS[slN++] = slV;
    }
    if (slN < 2) return ESP_FAIL;

    int32_t slMin = aslS[0], slMax = aslS[0], slSum = 0;
    for (int i = 0; i < slN; i++) {
        if (aslS[i] < slMin) slMin = aslS[i];
        if (aslS[i] > slMax) slMax = aslS[i];
        slSum += aslS[i];
    }
    if (slN == ADS1115_BURST_N)
        *pslRaw = (int16_t)((slSum - slMin - slMax) / (slN - 2));
    else
        *pslRaw = (int16_t)(slSum / slN);

    /* Spread in sensor-mV (LSB = FSR/2^15 / divider). */
    s_ulBurstNoiseMv = (uint32_t)((float)(slMax - slMin) *
                                  (ADS1115_FSR_V / ADS1115_RES) /
                                  HAL_VDIV_RATIO * 1000.0f + 0.5f);
    return ESP_OK;
}

static void hal_i2c_scan(void)
{
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "I2C-Scan (SDA=%d SCL=%d):",
            (int)HAL_I2C_SDA_GPIO, (int)HAL_I2C_SCL_GPIO);
    bool bFound = false;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "  device 0x%02X", addr);
            bFound = true;
        }
    }
    if (!bFound)
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "  no I2C device found — check wiring/pull-ups");
}

/* ============================================================================
 * Initialization
 * ========================================================================== */
static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = HAL_I2C_PORT,
        .sda_io_num                   = HAL_I2C_SDA_GPIO,
        .scl_io_num                   = HAL_I2C_SCL_GPIO,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (e != ESP_OK) return e;

    hal_i2c_scan();

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADS1115_ADDR,
        .scl_speed_hz    = 100000,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_ads1115);
}

static esp_err_t ssr_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = HAL_LEDC_MODE,
        .duty_resolution = HAL_LEDC_DUTY_BITS,
        .timer_num       = HAL_LEDC_TIMER,
        .freq_hz         = HAL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&timer_cfg);
    if (e != ESP_OK) return e;

    ledc_channel_config_t ch_cfg = {
        .speed_mode = HAL_LEDC_MODE,
        .channel    = HAL_LEDC_CHANNEL,
        .timer_sel  = HAL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = HAL_SSR_GPIO,
        .duty       = 0,            /* pump off (safe default) */
        .hpoint     = 0,
    };
    return ledc_channel_config(&ch_cfg);
}

static esp_err_t tsens_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t e = temperature_sensor_install(&cfg, &s_tsens);
    if (e != ESP_OK) return e;
    return temperature_sensor_enable(s_tsens);
}

/* --- On-board WS2812 RGB LED: force OFF at boot ------------------------------
 * The DevKitC-1 carries an addressable RGB LED (GPIO48 on v1.0 boards, GPIO38
 * on v1.1). A one-time DHT wiring scan (v4.27.3-.5) latched garbage into it
 * (solid white) and a WS2812 HOLDS its last frame as long as it is powered —
 * chip resets do not clear it. We do not use the LED, so blank both candidate
 * pins with an explicit all-zero frame at every boot. */
static portMUX_TYPE s_rgb_mux = portMUX_INITIALIZER_UNLOCKED;

/* IRAM: the WS2812 bit timing is sub-microsecond. Running from flash, a
 * cache stall between the level writes stretches T0H past the 0/1 threshold
 * and the "black" frame turns into garbage (that is exactly a lit-white
 * failure mode). IRAM + disabled interrupts makes the loop deterministic.
 * CAUTION cycle budget: this project runs at 160 MHz with DFS (80..160) —
 * timing MUST use the MEASURED ticks/us, never a hardcoded clock. A frame
 * timed for 240 MHz stretches every 0-bit past the threshold and paints the
 * LED solid WHITE (exactly the bug this replaces). */
static void IRAM_ATTR rgb_frame_send_zeros(uint32_t ulRegSet, uint32_t ulRegClr,
                                           uint32_t ulMask, int iBits,
                                           uint32_t ulTicksPerUs)
{
    const uint32_t ulHighCyc = (ulTicksPerUs * 35) / 100;   /* T0H ~0.35 us  */
    const uint32_t ulLowCyc  = (ulTicksPerUs * 90) / 100;   /* T0L ~0.90 us  */
    portENTER_CRITICAL(&s_rgb_mux);
    for (int i = 0; i < iBits; i++) {
        uint32_t ulStart = esp_cpu_get_cycle_count();
        REG_WRITE(ulRegSet, ulMask);
        while (esp_cpu_get_cycle_count() - ulStart < ulHighCyc) { }
        REG_WRITE(ulRegClr, ulMask);
        while (esp_cpu_get_cycle_count() - ulStart < ulHighCyc + ulLowCyc) { }
    }
    portEXIT_CRITICAL(&s_rgb_mux);
}

static void rgb_led_blank(gpio_num_t ePin)
{
    /* Direct W1TS/W1TC register writes, bank-aware (GPIO 0..31 vs 32..48). */
    const bool     bBank1   = (ePin >= 32);
    const uint32_t ulMask   = 1UL << (bBank1 ? (ePin - 32) : ePin);
    const uint32_t ulRegSet = bBank1 ? GPIO_OUT1_W1TS_REG : GPIO_OUT_W1TS_REG;
    const uint32_t ulRegClr = bBank1 ? GPIO_OUT1_W1TC_REG : GPIO_OUT_W1TC_REG;

    gpio_reset_pin(ePin);
    gpio_set_direction(ePin, GPIO_MODE_OUTPUT);
    gpio_set_level(ePin, 0);

    /* Pin the CPU frequency while bit-banging: DFS switching mid-frame
     * would silently rescale the cycle counter. */
    esp_pm_lock_handle_t hLock = NULL;
    bool bLocked =
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ws2812", &hLock) == ESP_OK
        && esp_pm_lock_acquire(hLock) == ESP_OK;

    /* 3 attempts x 4 chained LEDs worth of zero bits, each followed by a
     * >280 us LOW latch gap — belt and braces against a single bad frame. */
    for (int iTry = 0; iTry < 3; iTry++) {
        esp_rom_delay_us(300);
        rgb_frame_send_zeros(ulRegSet, ulRegClr, ulMask, 4 * 24,
                             esp_rom_get_cpu_ticks_per_us());
    }
    esp_rom_delay_us(300);

    if (bLocked) esp_pm_lock_release(hLock);
    if (hLock)   esp_pm_lock_delete(hLock);
    /* Leave the pin driven LOW so the LED input cannot pick up noise. */
}

/* All GPIOs that carry no function in this project, driven HIGH as outputs.
 * Defined levels (no floating inputs), and HIGH — not LOW — on purpose: the
 * board's status LEDs (TX/RX of the CH343 USB-serial bridge on 43/44, and
 * others) sit between 3V3 and the signal, i.e. ACTIVE-LOW. Grounding the
 * pins lit them all up (observed on this bench); HIGH keeps them dark.
 * NOT in this list: 4 DHT, 5 SSR, 7/8 I2C, 19/20 USB-JTAG, 26..37
 * flash/PSRAM, 0/3/45/46 strapping (left at reset defaults — the boot ROM
 * must be free to sample them), 21/38/48 WS2812 RGB candidates depending on
 * board revision (blanked + idle LOW, see rgb_led_blank). */
static void unused_pins_park_high(void)
{
    static const gpio_num_t aeUnused[] = {
        GPIO_NUM_1,  GPIO_NUM_2,  GPIO_NUM_6,  GPIO_NUM_9,  GPIO_NUM_10,
        GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15,
        GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41,
        GPIO_NUM_42, GPIO_NUM_43, GPIO_NUM_44, GPIO_NUM_47,
    };
    for (size_t i = 0; i < sizeof(aeUnused) / sizeof(aeUnused[0]); i++) {
        gpio_reset_pin(aeUnused[i]);
        gpio_set_direction(aeUnused[i], GPIO_MODE_OUTPUT);
        gpio_set_level(aeUnused[i], 1);
    }
}

bool hal_setup(void)
{
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "init");

    /* WS2812 sits on 48, 38 or 21 depending on board revision — blank all
     * three (0,0,0 = off), they stay driven LOW (proper data idle). */
    rgb_led_blank(GPIO_NUM_48);
    rgb_led_blank(GPIO_NUM_38);
    rgb_led_blank(GPIO_NUM_21);
    unused_pins_park_high();

    esp_err_t e = i2c_init();
    if (e != ESP_OK) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "I2C/ADS1115 init error=%d", (int)e);
        return false;
    }
    e = ssr_init();
    if (e != ESP_OK) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "SSR/LEDC init error=%d", (int)e);
        return false;
    }
    /* Internal temperature sensor is not critical -> only log the error. */
    e = tsens_init();
    if (e != ESP_OK)
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "internal temperature sensor unavailable (error=%d)", (int)e);

    return true;
}

/* ============================================================================
 * Public API
 * ========================================================================== */
bool hal_relay_set(bool bOn)
{
    uint32_t ulDuty = bOn ? HAL_LEDC_DUTY_MAX : 0;     /* on/off via duty 100%/0% */
    if (ledc_set_duty(HAL_LEDC_MODE, HAL_LEDC_CHANNEL, ulDuty) != ESP_OK) return false;
    if (ledc_update_duty(HAL_LEDC_MODE, HAL_LEDC_CHANNEL) != ESP_OK)      return false;
    s_relay_on = bOn;
    logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG, "relay_set(%d)", (int)bOn);
    return true;
}

bool hal_relay_get(bool *pbOutOn)
{
    if (!pbOutOn) return false;
    *pbOutOn = s_relay_on;
    return true;
}

bool hal_pressure_mv_read(uint32_t *pulOutMv)
{
    if (!pulOutMv) return false;
    int16_t slRaw = 0;
    if (ads1115_read_raw(&slRaw) != ESP_OK) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "ADS1115 read error");
        return false;
    }
    float flVAds    = (float)slRaw * (ADS1115_FSR_V / ADS1115_RES);  /* V at ADS pin */
    float flVSensor = flVAds / HAL_VDIV_RATIO;                       /* V at sensor  */
    if (flVSensor < 0.0f) flVSensor = 0.0f;
    *pulOutMv = (uint32_t)(flVSensor * 1000.0f + 0.5f);
    logging(LOG_TARGET_AUTO, DBG_LVL_VERBOSE, TAG,
            "ADS1115 raw=%d V=%.3f -> %u mV", (int)slRaw, flVSensor, (unsigned)*pulOutMv);
    return true;
}

/* Runtime-changeable sensor curve (work package 5): full scale keeps the
 * original hardware value (500 PSI = 34.47 bar) as its default, but range/
 * scale/offset come from the Fon_Sensor_* datapoints via this setter. */
static float s_flPressRangeBar   = PRESSURE_MAX_PSI * PSI_TO_BAR;
static float s_flPressScale      = 1.0f;
static float s_flPressOffsetBar  = 0.0f;

void hal_pressure_calibration_set(float flRangeBar, float flScale,
                                  int16_t sOffsetMbar)
{
    if (flRangeBar > 0.0f) s_flPressRangeBar = flRangeBar;
    if (flScale > 0.0f)    s_flPressScale = flScale;
    s_flPressOffsetBar = (float)sOffsetMbar / 1000.0f;
}

bool hal_pressure_read(uint32_t *pulOutMv, float *pflOutBar)
{
    uint32_t ulMv = 0;
    if (!hal_pressure_mv_read(&ulMv)) return false;
    if (pulOutMv) *pulOutMv = ulMv;

    if (pflOutBar) {
        float flV   = (float)ulMv / 1000.0f;
        float flBar = (flV - PRESSURE_V_MIN) / (PRESSURE_V_MAX - PRESSURE_V_MIN)
                      * s_flPressRangeBar;
        if (flBar < 0.0f)               flBar = 0.0f;
        if (flBar > s_flPressRangeBar)  flBar = s_flPressRangeBar;
        *pflOutBar = flBar * s_flPressScale + s_flPressOffsetBar;
    }
    return true;
}

bool hal_pressure_bar_read(float *pflOutBar)
{
    if (!pflOutBar) return false;
    return hal_pressure_read(NULL, pflOutBar);
}

bool hal_internal_temp_read(float *pflOutCelsius)
{
    if (!pflOutCelsius) return false;
    if (!s_tsens) { *pflOutCelsius = 0.0f; return false; }
    float flC = 0.0f;
    if (temperature_sensor_get_celsius(s_tsens, &flC) != ESP_OK) return false;
    *pflOutCelsius = flC;
    return true;
}

bool hal_usb_connected_get(bool *pbOutConnected)
{
    if (!pbOutConnected) return false;
    /* TODO: real VBUS detection. Placeholder: assume connected so that local
     * logs are visible during development. */
    *pbOutConnected = true;
    return true;
}
