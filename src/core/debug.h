/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* =============================================================
 * debug_modul — central log/debug output via the logging() macro.
 *
 * Levels (1 = most output, 3 = least):
 *   1 DBG_LVL_VERBOSE : high debugging, everything is output
 *   2 DBG_LVL_MEDIUM  : medium
 *   3 DBG_LVL_LOW     : low, only important things
 *   4 DBG_LVL_RES4    : placeholder (selectable later)
 *   5 DBG_LVL_RES5    : placeholder (selectable later)
 *
 * A message is output if its msg_level >= the current
 * threshold. Threshold 1 => everything, threshold 3 => level-3 only.
 * ============================================================= */

typedef enum {
    DBG_LVL_VERBOSE = 1,
    DBG_LVL_MEDIUM  = 2,
    DBG_LVL_LOW     = 3,
    DBG_LVL_RES4    = 4,
    DBG_LVL_RES5    = 5,
} debug_level_t;

/* Output target — additional first parameter of logging(). */
typedef enum {
    LOG_TARGET_AUTO = 0,   /* decide based on datapoint Logging_Allow_Remote      */
    LOG_TARGET_LOCAL,      /* local only (printf, when USB is plugged in)         */
    LOG_TARGET_REMOTE,     /* transmit to the Linux server only                  */
    LOG_TARGET_BOTH,       /* local AND remote                                   */
} log_target_t;

/* =============================================================
 * Datapoint DATA_LOGGING_ALLOW_REMOTE (uint8_t) — runtime config.
 *
 *   Bit 1-4 (low nibble)  : enable flags
 *       0x01 LOGGING_FLAG_REMOTE : remote logging to the server allowed
 *       0x02 LOGGING_FLAG_LOCAL  : local logging allowed (only with USB)
 *   Bit 5-8 (high nibble) : log level (debug_level_t, 1..5);
 *                           0 => module default (debug_level_get()).
 *
 * Example: 0x31 => level 3 (LOW) + remote active.
 *          0x12 => level 1 (VERBOSE) + local active.
 *          0x33 => level 3 + remote + local.
 * As long as the datapoint is 0 (unconfigured), logging() outputs
 * locally via printf to be safe, so that no messages are lost.
 * ============================================================= */
#define LOGGING_FLAG_REMOTE          0x01u
#define LOGGING_FLAG_LOCAL           0x02u
#define LOGGING_FLAGS_MASK           0x0Fu
#define LOGGING_LEVEL_SHIFT          4u

#define LOGGING_FLAGS_GET(cfg)       ((uint8_t)((cfg) & LOGGING_FLAGS_MASK))
#define LOGGING_LEVEL_FROM_CFG(cfg)  ((debug_level_t)((uint8_t)(cfg) >> LOGGING_LEVEL_SHIFT))
#define LOGGING_REMOTE_ALLOWED(cfg)  (((cfg) & LOGGING_FLAG_REMOTE) != 0u)
#define LOGGING_LOCAL_ALLOWED(cfg)   (((cfg) & LOGGING_FLAG_LOCAL)  != 0u)

bool          debug_init(debug_level_t eInitialLevel);
void          debug_level_set(debug_level_t eLevel);   /* default when datapoint level is 0 */
debug_level_t debug_level_get(void);

/* Injected console probe (dependency inversion — core knows no HAL);
 * main registers it. Optional (NULL = local output always allowed).
 * NOTE: the former remote PUSH sink was removed — remote diagnostics run
 * over the structured logging ring, pulled by the server via log_read. */
void debug_console_probe_set(bool (*pfnConsoleUp)(void));

/* The actual output function; called by the logging() macro.
 * pstrFile/slLine are NULL/0 when built without LOGGING_WITH_PROGRAMMCODE. */
void logging_output(log_target_t eTarget, debug_level_t eMsgLevel,
                    const char *pstrTag, const char *pstrFile, int slLine,
                    const char *pstrFmt, ...);

/* logging(target, msg_level, tag, fmt, ...) — adds file/line if
 * compiled with -DLOGGING_WITH_PROGRAMMCODE. */
#ifdef LOGGING_WITH_PROGRAMMCODE
#define logging(target, msg_level, tag, fmt, ...)                              \
    logging_output((target), (msg_level), (tag), __FILE__, __LINE__,           \
                   fmt, ##__VA_ARGS__)
#else
#define logging(target, msg_level, tag, fmt, ...)                              \
    logging_output((target), (msg_level), (tag), NULL, 0, fmt, ##__VA_ARGS__)
#endif
