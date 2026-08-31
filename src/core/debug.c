/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "debug.h"
#include "data_store.h"
#include "logging.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#define LOG_MSG_MAX   256
#define LOG_LINE_MAX  (LOG_MSG_MAX + 96)

static debug_level_t s_eLevel = DBG_LVL_VERBOSE;

/* Injected console probe (dependency inversion: core knows no HAL).
 * NULL -> local output always allowed. The former remote PUSH sink was
 * removed with work package 3: remote diagnostics run over the structured
 * logging ring, pulled by the server via log_read. */
static bool (*s_pfnConsoleUp)(void);

void debug_console_probe_set(bool (*pfnConsoleUp)(void))          { s_pfnConsoleUp = pfnConsoleUp; }

bool debug_init(debug_level_t eInitialLevel)
{
    s_eLevel = eInitialLevel;
    /* data_store may not be initialized yet here -> force local. */
    logging(LOG_TARGET_LOCAL, DBG_LVL_LOW, "debug", "init, level=%d", (int)eInitialLevel);
    return true;
}

void debug_level_set(debug_level_t eLevel) { s_eLevel = eLevel; }
debug_level_t debug_level_get(void) { return s_eLevel; }

/* IMPORTANT: logging() must NOT be called while the data_store mutex is
 * held (logging itself reads from the data_store). */
void logging_output(log_target_t eTarget, debug_level_t eMsgLevel,
                    const char *pstrTag, const char *pstrFile, int slLine,
                    const char *pstrFmt, ...)
{
    /* Read the config datapoint. Fails as long as data_store is not
     * initialized -> ulCfg stays 0 (= unconfigured). */
    uint32_t ulCfg = 0;
    bool bHaveCfg = data_store_u32_get(DATA_LOGGING_ALLOW_REMOTE, &ulCfg) && (ulCfg != 0);
    uint8_t ucCfg = (uint8_t)ulCfg;

    /* Threshold: high nibble of the datapoint, otherwise module default. */
    debug_level_t eThreshold = LOGGING_LEVEL_FROM_CFG(ucCfg);
    if ((int)eThreshold == 0) eThreshold = s_eLevel;

    /* Filter: emit the message only if its level >= threshold. */
    if ((int)eMsgLevel < (int)eThreshold) return;

    /* Format the payload text once. */
    char strMsg[LOG_MSG_MAX];
    va_list args;
    va_start(args, pstrFmt);
    vsnprintf(strMsg, sizeof(strMsg), pstrFmt, args);
    va_end(args);

    /* Facade into the structured logging ring (work package 3): the legacy
     * levels map LOW->INFO, MEDIUM->DEBUG, VERBOSE->TRACE; the tag travels
     * in the (truncated) text. Emit has its own fast gate + never logs. */
    {
        static const log_level_t aeMap[] = {
            [DBG_LVL_VERBOSE] = LOG_LEVEL_TRACE,
            [DBG_LVL_MEDIUM]  = LOG_LEVEL_DEBUG,
            [DBG_LVL_LOW]     = LOG_LEVEL_INFO,
        };
        log_level_t eLvl = ((unsigned)eMsgLevel <= DBG_LVL_LOW)
                               ? aeMap[eMsgLevel] : LOG_LEVEL_INFO;
        if (logging_is_enabled_fast(eLvl)) {
            char strRec[LOG_MAX_TEXT];
            /* Bounded precisions keep the line within the record text. */
            snprintf(strRec, sizeof(strRec), "%.14s: %.31s", pstrTag, strMsg);
            logging_emit(eLvl, LOG_MOD_APP, LOG_EVT_TEXT, NULL, 0, strRec);
        }
    }

    /* Build the complete line; optionally with file:line (source location). */
    char strLine[LOG_LINE_MAX];
    if (pstrFile) {
        const char *pstrBase = strrchr(pstrFile, '/');
        pstrBase = pstrBase ? pstrBase + 1 : pstrFile;
        snprintf(strLine, sizeof(strLine), "[L%d][%s][%s:%d] %s",
                 (int)eMsgLevel, pstrTag, pstrBase, slLine, strMsg);
    } else {
        snprintf(strLine, sizeof(strLine), "[L%d][%s] %s",
                 (int)eMsgLevel, pstrTag, strMsg);
    }

    /* Unconfigured (datapoint = 0): output locally to be safe. */
    if (!bHaveCfg) {
        printf("%s\n", strLine);
        return;
    }

    /* Desired targets from the parameter, always limited by the flags. */
    bool bWantLocal  = false;
    bool bWantRemote = false;
    switch (eTarget) {
    case LOG_TARGET_LOCAL:  bWantLocal  = LOGGING_LOCAL_ALLOWED(ucCfg);  break;
    case LOG_TARGET_REMOTE: bWantRemote = LOGGING_REMOTE_ALLOWED(ucCfg); break;
    case LOG_TARGET_BOTH:
        bWantLocal  = LOGGING_LOCAL_ALLOWED(ucCfg);
        bWantRemote = LOGGING_REMOTE_ALLOWED(ucCfg);
        break;
    case LOG_TARGET_AUTO:
    default:
        bWantLocal  = LOGGING_LOCAL_ALLOWED(ucCfg);
        bWantRemote = LOGGING_REMOTE_ALLOWED(ucCfg);
        break;
    }

    /* Output locally while a console is available (probe injected by main;
     * without a probe local output is always allowed). The legacy REMOTE
     * target degrades to local output — the structured pull channel
     * (logging ring + log_read) is the remote path since work package 3. */
    if (bWantLocal || bWantRemote) {
        if (!s_pfnConsoleUp || s_pfnConsoleUp()) printf("%s\n", strLine);
    }
}
