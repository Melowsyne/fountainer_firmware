/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "logging.h"
#include "log_ring.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_random.h"

/* Static byte ring (logger_datenstruktuer.md): variable-length records
 * instead of fixed 84-byte slots — within the same 32 KiB budget this fits
 * roughly 700–1100 records instead of 390. boot_id is ring metadata, not part
 * of the record (design spec §27); externally log_record_t remains the
 * copy-out type so the flash format and protocol stay byte-identical. */
_Static_assert(LOG_RING_BYTES <= 0xFF00, "log ring offsets are u16");
_Static_assert(LOG_RING_MAX_TEXT + 1 == sizeof(((log_record_t *)0)->strText),
               "ring text must round-trip into log_record_t");

static uint8_t    s_aucRingBuf[LOG_RING_BYTES];
static log_ring_t s_stRing;
static uint32_t   s_ulBootId;

/* Config flags read in the hot gate — plain loads, changed rarely. */
static volatile bool    s_bEnabled = true;
static volatile uint8_t s_ucRuntimeLevel = LOG_LEVEL_INFO;

/* Short critical section around ring state (emit may run on both cores). */
static portMUX_TYPE s_xLock = portMUX_INITIALIZER_UNLOCKED;

void logging_init_early(void)
{
    s_ulBootId = esp_random();
    if (s_ulBootId == 0) s_ulBootId = 1;   /* 0 is "unknown" on the wire */
    log_ring_init(&s_stRing, s_aucRingBuf, (uint16_t)LOG_RING_BYTES);
}

bool logging_is_enabled_fast(log_level_t eLevel)
{
    /* STORE GATE (re-enabled 23 Aug): Log_Runtime_Level once again limits
     * what is stored in the RAM ring (default INFO). Background: the full
     * capture of 16 Aug (all levels) produced ~16 records/s — the ring
     * (~800 records) overflowed continuously between the 60 s server polls,
     * 14-95 %% of the records were lost as gaps (Log_Dropped in the
     * millions). DEBUG/TRACE remain RETRIEVABLE: set Log_Runtime_Level
     * to 4/5 at runtime via dp_write, and everything is stored
     * again. */
    return s_bEnabled && eLevel != LOG_LEVEL_OFF &&
           (uint8_t)eLevel <= s_ucRuntimeLevel;
}

void logging_emit(log_level_t eLevel, uint8_t ucModuleId, uint16_t usEventId,
                  const int32_t *pslArgs, uint8_t ucArgc, const char *pstrText)
{
    if (!logging_is_enabled_fast(eLevel)) return;
    if (ucArgc > LOG_MAX_ARGS) ucArgc = LOG_MAX_ARGS;

    uint32_t ulUptimeMs = (uint32_t)(esp_timer_get_time() / 1000ULL);

    /* Serialization + bounded eviction under the short lock; the ring
     * assigns the seq. */
    portENTER_CRITICAL(&s_xLock);
    uint32_t ulSeq = log_ring_push(&s_stRing, (uint8_t)eLevel, ucModuleId,
                                   usEventId, ulUptimeMs, pslArgs, ucArgc,
                                   pstrText);
    portEXIT_CRITICAL(&s_xLock);

    /* Flash tier (non-blocking enqueue; no-op without the partition) —
     * unchanged logf_entry_t format, its own level filter. */
    log_record_t stRec;
    memset(&stRec, 0, sizeof(stRec));
    stRec.ulSeq      = ulSeq;
    stRec.ulBootId   = s_ulBootId;
    stRec.ulUptimeMs = ulUptimeMs;
    stRec.usEventId  = usEventId;
    stRec.ucModuleId = ucModuleId;
    stRec.ucLevel    = (uint8_t)eLevel;
    stRec.ucArgc     = ucArgc;
    if (pslArgs && ucArgc)
        memcpy(stRec.aslArgs, pslArgs, ucArgc * sizeof(int32_t));
    if (pstrText)
        strlcpy(stRec.strText, pstrText, sizeof(stRec.strText));
    logging_flash_submit(&stRec);
}

static void stats_fill_locked(log_stats_t *pstStats)
{
    pstStats->ulBootId  = s_ulBootId;
    pstStats->ulNextSeq = log_ring_next_seq(&s_stRing);
    pstStats->ulDropped = log_ring_overwritten(&s_stRing);
    pstStats->ulFirstSeqAvailable = log_ring_first_seq(&s_stRing);
}

size_t logging_read_since(uint32_t ulSinceSeq, log_level_t eMinLevel,
                          log_record_t *pstOut, size_t szMax,
                          log_stats_t *pstStats)
{
    if (!pstOut || szMax == 0) {
        if (pstStats) logging_stats_get(pstStats);
        return 0;
    }
    if (eMinLevel == LOG_LEVEL_OFF) eMinLevel = LOG_LEVEL_TRACE; /* no filter */

    size_t szCopied = 0;
    portENTER_CRITICAL(&s_xLock);
    log_ring_iter_t stIt;
    log_ring_view_t stView;
    log_ring_iter_begin(&s_stRing, &stIt);
    log_ring_iter_seek(&stIt, ulSinceSeq);      /* header hops, cheap      */
    while (szCopied < szMax && log_ring_iter_next(&stIt, &stView)) {
        if (stView.level > (uint8_t)eMinLevel) continue;
        log_record_t *pstRec = &pstOut[szCopied++];
        memset(pstRec, 0, sizeof(*pstRec));
        pstRec->ulSeq      = stView.seq;
        pstRec->ulBootId   = s_ulBootId;
        pstRec->ulUptimeMs = stView.uptime_ms;
        pstRec->usEventId  = stView.event_id;
        pstRec->ucModuleId = stView.module;
        pstRec->ucLevel    = stView.level;
        pstRec->ucArgc     = stView.argc;
        memcpy(pstRec->aslArgs, stView.args,
               stView.argc * sizeof(int32_t));
        strlcpy(pstRec->strText, stView.text, sizeof(pstRec->strText));
    }
    if (pstStats) stats_fill_locked(pstStats);
    portEXIT_CRITICAL(&s_xLock);
    return szCopied;
}

void logging_stats_get(log_stats_t *pstStats)
{
    if (!pstStats) return;
    portENTER_CRITICAL(&s_xLock);
    stats_fill_locked(pstStats);
    portEXIT_CRITICAL(&s_xLock);
}

void logging_clear_runtime(void)
{
    portENTER_CRITICAL(&s_xLock);
    log_ring_clear(&s_stRing);  /* seq keeps counting — the server sees the gap */
    portEXIT_CRITICAL(&s_xLock);
}

void logging_set_enabled(bool bEnabled)          { s_bEnabled = bEnabled; }

void logging_set_runtime_level(log_level_t eLevel)
{
    /* Once again controls the STORE GATE of the RAM ring (records with a
     * level above it are not stored) — see logging_is_enabled_fast. */
    if (eLevel > LOG_LEVEL_TRACE) eLevel = LOG_LEVEL_TRACE;
    s_ucRuntimeLevel = (uint8_t)eLevel;
}

/* The flash tier (previous-boot slot, ack, level) lives in logging_flash.c. */
