/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

/* =============================================================
 * link_score — PURE link-quality scoring (Link_Robustness_v1 §1).
 * No I/O, no FreeRTOS: counters/RSSI in, score/state out — the
 * host test drives this file directly (like pump_manager).
 *
 * score = rssi_base(-60 dBm -> 100 … -90 dBm -> 0, linear)
 *       - 15 per WLAN disconnect   (last hour,  capped -30)
 *       - 10 per session drop      (last hour,  capped -30)
 *       -  2 per dropped TX frame  (last 5 min, capped -20)
 * EMA-smoothed (alpha 0.3); POOR below 40, GOOD again above 55.
 * ============================================================= */

#define LINK_SCORE_POOR_BELOW   40
#define LINK_SCORE_GOOD_ABOVE   55
#define LINK_SCORE_EMA_ALPHA    0.3f

typedef struct {
    int8_t   rssi_dbm;             /* current RSSI (0 = no link/unknown)   */
    uint8_t  wlan_disconnects_1h;  /* WLAN drops within the last hour      */
    uint8_t  session_drops_1h;     /* WS session losses within the hour    */
    uint16_t tx_fails_5m;          /* dropped frames within 5 minutes      */
} link_inputs_t;

typedef struct {
    float score_ema;               /* smoothed score 0..100                */
    bool  poor;                    /* hysteresis state                     */
} link_state_t;

void link_state_init(link_state_t *st);

/* One scoring step: computes the raw score from the inputs, folds it into
 * the EMA and applies the POOR/GOOD hysteresis. Returns the raw score. */
uint8_t link_score_step(link_state_t *st, const link_inputs_t *in);
