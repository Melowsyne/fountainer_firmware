/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* Host tests of the PURE link scorer (Link_Robustness_v1 §1): RSSI base
 * curve, penalty caps, EMA smoothing and the POOR/GOOD hysteresis. */

#include <stdio.h>
#include "link_score.h"

static int s_slFails;
#define CHECK(cond, name)                                             \
    do { if (cond) { printf("PASS %s\n", name); }                     \
         else { printf("FAIL %s (line %d)\n", name, __LINE__);        \
                s_slFails++; } } while (0)

static uint8_t step_n(link_state_t *st, link_inputs_t in, int n)
{
    uint8_t raw = 0;
    for (int i = 0; i < n; i++) raw = link_score_step(st, &in);
    return raw;
}

int main(void)
{
    link_state_t st;

    /* RSSI base curve. */
    link_state_init(&st);
    link_inputs_t in = { .rssi_dbm = -55 };
    CHECK(link_score_step(&st, &in) == 100, "rssi -55 -> raw 100");
    in.rssi_dbm = -90;
    CHECK(link_score_step(&st, &in) == 0,   "rssi -90 -> raw 0");
    in.rssi_dbm = -75;
    CHECK(link_score_step(&st, &in) == 50,  "rssi -75 -> raw 50 (linear)");
    in.rssi_dbm = 0;
    CHECK(link_score_step(&st, &in) == 0,   "no link -> raw 0");

    /* Penalties and caps. */
    link_state_init(&st);
    in = (link_inputs_t){ .rssi_dbm = -55, .wlan_disconnects_1h = 1 };
    CHECK(link_score_step(&st, &in) == 85,  "1 wlan drop -> -15");
    in.wlan_disconnects_1h = 5;
    CHECK(link_score_step(&st, &in) == 70,  "wlan drops capped at -30");
    in = (link_inputs_t){ .rssi_dbm = -55, .session_drops_1h = 4 };
    CHECK(link_score_step(&st, &in) == 70,  "session drops capped at -30");
    in = (link_inputs_t){ .rssi_dbm = -55, .tx_fails_5m = 30 };
    CHECK(link_score_step(&st, &in) == 80,  "tx fails capped at -20");
    in = (link_inputs_t){ .rssi_dbm = -90, .wlan_disconnects_1h = 9,
                          .session_drops_1h = 9, .tx_fails_5m = 99 };
    CHECK(link_score_step(&st, &in) == 0,   "floor clamps at 0");

    /* Hysteresis: good link stays GOOD; degradation flips to POOR only
     * once the EMA sinks below 40; recovery needs > 55. */
    link_state_init(&st);
    in = (link_inputs_t){ .rssi_dbm = -55 };
    step_n(&st, in, 5);
    CHECK(!st.poor, "healthy link stays GOOD");
    in = (link_inputs_t){ .rssi_dbm = -85, .session_drops_1h = 3 };  /* raw ~0 */
    step_n(&st, in, 2);
    CHECK(!st.poor, "EMA delays the flip (2 bad ticks not enough)");
    step_n(&st, in, 6);
    CHECK(st.poor, "sustained bad link -> POOR");
    in = (link_inputs_t){ .rssi_dbm = -62 };          /* raw ~93, drops aged */
    step_n(&st, in, 2);
    CHECK(st.poor, "hysteresis: brief recovery does not flip back");
    step_n(&st, in, 8);
    CHECK(!st.poor, "sustained recovery -> GOOD again");

    /* EMA starts optimistic (no false POOR on boot without wifi yet). */
    link_state_init(&st);
    in = (link_inputs_t){ .rssi_dbm = 0 };
    link_score_step(&st, &in);
    CHECK(!st.poor, "single no-link tick right after boot stays GOOD");

    if (s_slFails) { printf("%d TEST(S) FAILED\n", s_slFails); return 1; }
    printf("ALL TESTS PASSED (0 errors)\n");
    return 0;
}
