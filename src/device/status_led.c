/*
 * Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 *
 * This source code is proprietary. No license is granted to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of
 * this software without prior written permission from the copyright holder.
 *
 * For licensing inquiries, contact: info@melowsyne.com
 */

#include "status_led.h"
#include "hal.h"
#include "debug.h"

#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define TAG "status_led"

#define LED_BOOT_SHOW_US   (5 * 1000 * 1000)  /* both LEDs on after power-up */
#define LED_BLINK_HALF_US  (500 * 1000)       /* 1-s cadence: 500 ms on/off  */

/* ACTIVE LOW: 3V3 -> series resistor -> LED -> GPIO. */
#define LED_ON  0
#define LED_OFF 1

static bool               s_bPresent;      /* board has LED pins            */
static volatile bool      s_bBootPhase;    /* 5-s display still running     */
static volatile bool      s_bCommActive;   /* session to the server is up   */
static volatile bool      s_bRelayOn;      /* SSR output switched on        */
static volatile bool      s_bBlinkLevel;   /* current blink level, green    */
static esp_timer_handle_t s_hBootTimer;    /* one-shot, 5 s                 */
static esp_timer_handle_t s_hBlinkTimer;   /* periodic, 500 ms              */
/* Protects the LED state transitions between the pump task (relay), the
 * event-dispatch task (comm) and the esp_timer task (callbacks). */
static portMUX_TYPE       s_stLock = portMUX_INITIALIZER_UNLOCKED;

static void led_set(gpio_num_t ePin, bool bOn)
{
    gpio_set_level(ePin, bOn ? LED_ON : LED_OFF);
}

/* Green logic (descending priority), caller holds s_stLock:
 *   1. boot display (5 s)          -> on (done by boot_timer_cb)
 *   2. SSR switched on             -> blinks at 1-s cadence
 *   3. server session up           -> steady light
 *   4. otherwise                   -> off */
static void green_apply_locked(void)
{
    if (s_bBootPhase) return;                 /* boot display has priority */
    if (s_bRelayOn) {
        if (!esp_timer_is_active(s_hBlinkTimer)) {
            s_bBlinkLevel = true;
            led_set(hal_pins()->eLedGreen, true);
            esp_timer_start_periodic(s_hBlinkTimer, LED_BLINK_HALF_US);
        }
    } else {
        esp_timer_stop(s_hBlinkTimer);
        led_set(hal_pins()->eLedGreen, s_bCommActive);
    }
}

static void blink_timer_cb(void *pvArg)
{
    (void)pvArg;
    taskENTER_CRITICAL(&s_stLock);
    s_bBlinkLevel = !s_bBlinkLevel;
    led_set(hal_pins()->eLedGreen, s_bBlinkLevel);
    taskEXIT_CRITICAL(&s_stLock);
}

static void boot_timer_cb(void *pvArg)
{
    (void)pvArg;
    taskENTER_CRITICAL(&s_stLock);
    s_bBootPhase = false;
    led_set(hal_pins()->eLedRed, false);
    led_set(hal_pins()->eLedGreen, false);
    green_apply_locked();                     /* apply the remembered state */
    taskEXIT_CRITICAL(&s_stLock);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "boot display done");
}

bool status_led_init(void)
{
    const hal_pins_t *pstPins = hal_pins();
    s_bPresent = (pstPins->eLedRed != GPIO_NUM_NC &&
                  pstPins->eLedGreen != GPIO_NUM_NC);
    if (!s_bPresent) return true;             /* devkit: no status LEDs    */

    const gpio_num_t aePins[] = { pstPins->eLedRed, pstPins->eLedGreen };
    for (size_t i = 0; i < 2; i++) {
        gpio_reset_pin(aePins[i]);
        /* Set the level BEFORE switching direction — no off-glitch. */
        gpio_set_level(aePins[i], LED_ON);
        gpio_set_direction(aePins[i], GPIO_MODE_OUTPUT);
    }
    s_bBootPhase = true;

    const esp_timer_create_args_t stBoot = {
        .callback = boot_timer_cb, .name = "led_boot",
    };
    const esp_timer_create_args_t stBlink = {
        .callback = blink_timer_cb, .name = "led_blink",
    };
    if (esp_timer_create(&stBoot, &s_hBootTimer) != ESP_OK ||
        esp_timer_create(&stBlink, &s_hBlinkTimer) != ESP_OK) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "timer create failed");
        return false;
    }
    esp_timer_start_once(s_hBootTimer, LED_BOOT_SHOW_US);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "init: boot display 5 s (red=IO%d green=IO%d)",
            (int)pstPins->eLedRed, (int)pstPins->eLedGreen);
    return true;
}

void status_led_comm_active(bool bActive)
{
    if (!s_bPresent) return;
    taskENTER_CRITICAL(&s_stLock);
    s_bCommActive = bActive;
    green_apply_locked();
    taskEXIT_CRITICAL(&s_stLock);
}

void status_led_relay_active(bool bOn)
{
    /* Idempotent and cheap: hal_relay_set calls this in every pump cycle. */
    if (!s_bPresent || bOn == s_bRelayOn) return;
    taskENTER_CRITICAL(&s_stLock);
    s_bRelayOn = bOn;
    green_apply_locked();
    taskEXIT_CRITICAL(&s_stLock);
}
