/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

/* =============================================================
 * command_modul — predefined commands.
 *
 * Design pattern: COMMAND (commands as objects) + FACADE
 * (the single place that invokes the control functions).
 * Call chain: command_modul -> fountain_controlling -> hal.
 * ============================================================= */

typedef enum {
    CMD_RELAY_ON = 0,       /* set_state target_state=On      */
    CMD_RELAY_OFF,          /* set_state target_state=Off     */
    CMD_RELAY_ON_DURATION,  /* turn_on_duration               */
    CMD_AUTO,               /* set_state target_state=Auto    */
    CMD_MANUAL,             /* set_state target_state=Manual  */
    CMD_RESTART,            /* restart (pump)                 */
    CMD_REBOOT,             /* reboot (controller restart)    */
    CMD_WD_FAULT,           /* wd_fault (TEST ONLY): block the measure cycle
                               for <steps> seconds to exercise the watchdog */
} command_id_t;

typedef struct {
    command_id_t eId;
    uint32_t     ulArg;     /* e.g. duration_steps (30 s each) */
} command_t;

typedef struct {
    bool bRelayOn;
    bool bIsAuto;
    bool bOk;
} command_result_t;

bool command_init(void);

/* The single execution point. Delegates to fountain_controlling/hal. */
bool command_execute(const command_t *pstCmd, command_result_t *pstOut);

/* Maps a protocol control action onto a command_t. Vocabulary per spec:
 *   set_state (+ target_state On|Off|Auto|Manual), turn_on_duration
 *   (+ duration_steps), restart, reboot. */
bool command_protocol_map(const char *pstrCommand, const char *pstrTargetState,
                          uint32_t ulDurationSteps, command_t *pstOut);
