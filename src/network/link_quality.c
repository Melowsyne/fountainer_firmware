/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "link_quality.h"
#include "link_score.h"
#include "wlan_com.h"
#include "datapoints.h"
#include "event_manager.h"
#include "logging.h"
#include "debug.h"
#include "fountain_proto.h"

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#define TAG "link"

#define LQ_EVENT_RING     16           /* timestamps per sliding window     */
#define LQ_WIN_HOUR_S     3600
#define LQ_WIN_5MIN_S     300
#define LQ_SAMPLE_GOOD_S  60           /* NET_SAMPLE cadence                */
#define LQ_SAMPLE_POOR_S  120          /* throttled on a poor link (§B2)    */

typedef struct {
    uint32_t aulTs[LQ_EVENT_RING];
    uint8_t  ucIdx;
} lq_ring_t;

static link_state_t s_stScore;
static lq_ring_t s_stWlanDrops, s_stSessDrops, s_stTxFails;
static uint32_t s_ulPrevWlanDrops;     /* deltas of the cumulative counters */
static uint32_t s_ulPrevTxFail;
static uint32_t s_ulSessionDrops;      /* cumulative (DP)                   */
static volatile bool s_bSessionUp;
static uint32_t s_ulOfflineStartS;     /* uptime when the session dropped   */
static uint32_t s_ulOfflineTotalS;
static uint32_t s_ulLastOfflineS;
static uint32_t s_ulLastSampleS;
static bool s_bPoorReported;

static uint32_t up_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }

static void ring_push(lq_ring_t *pstR, uint32_t ulNow)
{
    pstR->aulTs[pstR->ucIdx] = ulNow ? ulNow : 1;
    pstR->ucIdx = (pstR->ucIdx + 1) % LQ_EVENT_RING;
}

static uint8_t ring_count(const lq_ring_t *pstR, uint32_t ulNow, uint32_t ulWinS)
{
    uint8_t ucN = 0;
    for (int i = 0; i < LQ_EVENT_RING; i++)
        if (pstR->aulTs[i] && ulNow - pstR->aulTs[i] <= ulWinS) ucN++;
    return ucN;
}

/* Session edges (event-manager dispatch context — keep it short). */
static void on_session_edge(system_event_t eEvent, const void *pvData, size_t szSize)
{
    (void)pvData; (void)szSize;
    uint32_t ulNow = up_s();
    if (eEvent == EVT_SESSION_READY) {
        if (!s_bSessionUp && s_ulOfflineStartS)
            s_ulLastOfflineS = ulNow - s_ulOfflineStartS;
        s_bSessionUp = true;
    } else {                       /* EVT_SESSION_LOST */
        s_bSessionUp = false;
        s_ulOfflineStartS = ulNow;
        s_ulSessionDrops++;
        ring_push(&s_stSessDrops, ulNow);
    }
}

bool link_quality_init(void)
{
    link_state_init(&s_stScore);
    event_manager_subscribe(EVT_SESSION_READY, on_session_edge);
    event_manager_subscribe(EVT_SESSION_LOST,  on_session_edge);
    return true;
}

bool link_quality_poor_get(void) { return s_stScore.poor; }

/* TEST ONLY: forces POOR until the deadline (evaluated in the tick). */
static volatile uint32_t s_ulTestPoorUntilS;

void link_quality_test_poor_set(uint32_t ulSeconds)
{
    s_ulTestPoorUntilS = up_s() + ulSeconds;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "TEST: forcing POOR for %u s", (unsigned)ulSeconds);
}

void link_quality_tick(void)
{
    uint32_t ulNow = up_s();

    /* Fold the cumulative counters into the sliding windows. */
    uint32_t ulWlan = wlan_com_disconnect_count_get();
    for (; s_ulPrevWlanDrops < ulWlan; s_ulPrevWlanDrops++)
        ring_push(&s_stWlanDrops, ulNow);
    uint32_t ulTxOk = 0, ulTxFail = 0;
    fountain_proto_tx_stats(&ulTxOk, &ulTxFail);
    for (; s_ulPrevTxFail < ulTxFail; s_ulPrevTxFail++)
        ring_push(&s_stTxFails, ulNow);

    /* Offline time accumulates while no session is up. */
    if (!s_bSessionUp) s_ulOfflineTotalS += 5;   /* monitor tick period */

    link_inputs_t stIn = { .rssi_dbm = 0 };
    wifi_ap_record_t stAp;
    if (esp_wifi_sta_get_ap_info(&stAp) == ESP_OK)
        stIn.rssi_dbm = stAp.rssi;
    stIn.wlan_disconnects_1h = ring_count(&s_stWlanDrops, ulNow, LQ_WIN_HOUR_S);
    stIn.session_drops_1h    = ring_count(&s_stSessDrops, ulNow, LQ_WIN_HOUR_S);
    stIn.tx_fails_5m         = ring_count(&s_stTxFails,  ulNow, LQ_WIN_5MIN_S);

    bool bWasPoor = s_stScore.poor;
    uint8_t ucRaw = link_score_step(&s_stScore, &stIn);
    (void)ucRaw;
    if (s_ulTestPoorUntilS) {                 /* injected test degradation */
        if (ulNow < s_ulTestPoorUntilS) {
            s_stScore.poor = true;
            s_stScore.score_ema = 10.0f;      /* keeps hysteresis in POOR  */
        } else {
            s_ulTestPoorUntilS = 0;           /* natural recovery follows  */
        }
    }
    uint8_t ucScore = (uint8_t)s_stScore.score_ema;

    /* Mirror the Net_* datapoints. */
    dp_lock(portMAX_DELAY);
    DP_REF(Net_Link_Score)      = ucScore;
    DP_REF(Net_Link_State)      = s_stScore.poor ? 1 : 0;
    DP_REF(Net_Session_Drops)   = s_ulSessionDrops;
    DP_REF(Net_Send_Fail_Count) = ulTxFail;
    DP_REF(Net_Offline_Seconds) = s_ulOfflineTotalS;
    DP_REF(Net_Last_Offline_S)  = s_ulLastOfflineS;
    dp_unlock();

    /* Edge: POOR entered/left -> record (+ alert when reachable) + event. */
    if (s_stScore.poor != bWasPoor) {
        uint8_t ucPoor = s_stScore.poor ? 1 : 0;
        event_manager_publish(EVT_LINK_STATE_CHANGED, &ucPoor, sizeof(ucPoor));
        if (s_stScore.poor) {
            LOG_EMIT2(LOG_LEVEL_WARN, LOG_MOD_WLAN, LOG_EVT_LINK_POOR,
                      ucScore, stIn.rssi_dbm, "link poor");
            if (s_bSessionUp && !s_bPoorReported) {
                s_bPoorReported = true;
                fountain_proto_alert_send("link_poor", "warning",
                                          "Net_Link_Score", ucScore,
                                          LINK_SCORE_POOR_BELOW,
                                          "wlan link quality degraded");
            }
        } else {
            s_bPoorReported = false;
            LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_WLAN, LOG_EVT_LINK_RECOVERED,
                      ucScore, stIn.rssi_dbm, "link recovered");
        }
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "state -> %s (score %u)",
                s_stScore.poor ? "POOR" : "GOOD", ucScore);
    }

    /* Time-series record: the link-quality HISTORY travels over the log
     * pull (survives offline phases in the ring/flash tier). */
    uint32_t ulRate = s_stScore.poor ? LQ_SAMPLE_POOR_S : LQ_SAMPLE_GOOD_S;
    if (ulNow - s_ulLastSampleS >= ulRate) {
        s_ulLastSampleS = ulNow;
        LOG_EMIT4(LOG_LEVEL_INFO, LOG_MOD_WLAN, LOG_EVT_NET_SAMPLE,
                  stIn.rssi_dbm, ucScore, ulTxFail, s_ulSessionDrops, NULL);
    }
}
