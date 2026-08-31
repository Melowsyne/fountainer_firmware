/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "link_score.h"

void link_state_init(link_state_t *st)
{
    st->score_ema = 100.0f;        /* start optimistic: no penalty history */
    st->poor = false;
}

static int rssi_base(int8_t rssi)
{
    if (rssi == 0)   return 0;     /* no link / unknown = worst            */
    if (rssi >= -60) return 100;
    if (rssi <= -90) return 0;
    return (int)((rssi + 90) * 100 / 30);      /* linear -90..-60 -> 0..100 */
}

static int cap(int v, int lo) { return v < lo ? lo : v; }

uint8_t link_score_step(link_state_t *st, const link_inputs_t *in)
{
    int score = rssi_base(in->rssi_dbm);
    score += cap(-15 * (int)in->wlan_disconnects_1h, -30);
    score += cap(-10 * (int)in->session_drops_1h,    -30);
    score += cap( -2 * (int)in->tx_fails_5m,         -20);
    if (score < 0)   score = 0;
    if (score > 100) score = 100;

    st->score_ema = LINK_SCORE_EMA_ALPHA * (float)score +
                    (1.0f - LINK_SCORE_EMA_ALPHA) * st->score_ema;

    if (st->poor) {
        if (st->score_ema > LINK_SCORE_GOOD_ABOVE) st->poor = false;
    } else {
        if (st->score_ema < LINK_SCORE_POOR_BELOW) st->poor = true;
    }
    return (uint8_t)score;
}
