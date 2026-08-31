/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "command.h"
#include "pump_task.h"
#include "hal.h"
#include "system.h"
#include "task_measure.h"
#include "debug.h"
#include <string.h>

#define TAG "command"
#define DURATION_STEP_S 30u

bool command_init(void)
{
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "init");
    return true;
}

bool command_protocol_map(const char *pstrCmd, const char *pstrTarget,
                          uint32_t ulSteps, command_t *pstOut)
{
    if (!pstrCmd || !pstOut) return false;

    if (!strcmp(pstrCmd, "set_state")) {
        /* target_state per spec: On | Off | Auto | Manual (case-sensitive). */
        if      (pstrTarget && !strcmp(pstrTarget, "On"))     *pstOut = (command_t){CMD_RELAY_ON, 0};
        else if (pstrTarget && !strcmp(pstrTarget, "Off"))    *pstOut = (command_t){CMD_RELAY_OFF, 0};
        else if (pstrTarget && !strcmp(pstrTarget, "Auto"))   *pstOut = (command_t){CMD_AUTO, 0};
        else if (pstrTarget && !strcmp(pstrTarget, "Manual")) *pstOut = (command_t){CMD_MANUAL, 0};
        else { logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "set_state: unknown target_state: %s",
                       pstrTarget ? pstrTarget : "(missing)"); return false; }
    }
    else if (!strcmp(pstrCmd, "turn_on_duration")) *pstOut = (command_t){CMD_RELAY_ON_DURATION, ulSteps};
    else if (!strcmp(pstrCmd, "restart"))          *pstOut = (command_t){CMD_RESTART, 0};
    else if (!strcmp(pstrCmd, "reboot"))           *pstOut = (command_t){CMD_REBOOT, 0};
    else if (!strcmp(pstrCmd, "wd_fault"))         *pstOut = (command_t){CMD_WD_FAULT, ulSteps};
    else { logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "unknown command: %s", pstrCmd); return false; }
    return true;
}

bool command_execute(const command_t *pstCmd, command_result_t *pstOut)
{
    if (!pstCmd) return false;
    command_result_t stR = {0};
    bool bOk = false;

    switch (pstCmd->eId) {
    case CMD_RELAY_ON:
        bOk = pump_request_on(0);
        break;
    case CMD_RELAY_OFF:
        bOk = pump_request_off();
        break;
    case CMD_RELAY_ON_DURATION:
        bOk = pump_request_on(pstCmd->ulArg * DURATION_STEP_S);
        break;
    case CMD_AUTO:
        bOk = pump_mode_set(PM_MODE_AUTO);
        stR.bIsAuto = true;
        break;
    case CMD_MANUAL:
        bOk = pump_mode_set(PM_MODE_MANUAL);
        break;
    case CMD_RESTART:
        bOk = pump_request_restart();
        break;
    case CMD_REBOOT:
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "device reboot requested");
        pump_request_off();              /* relay safely OFF immediately */
        /* Deferred via core/system: command_result goes out first, then the
         * main-injected pre-reboot hook (clean WLAN teardown) + esp_restart. */
        bOk = system_reboot_deferred(800);
        break;
    case CMD_WD_FAULT:
        /* TEST ONLY (signed command): injects a one-shot block into the
         * measure cycle so the WD_MEASURE channel/TWDT can be exercised. */
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "wd_fault: blocking measure cycle for %u s (TEST)",
                (unsigned)pstCmd->ulArg);
        task_measure_test_block_set((uint32_t)pstCmd->ulArg);
        bOk = true;
        break;
    default:
        bOk = false;
        break;
    }

    stR.bOk = bOk;
    bool bRelayOn = false;
    if (hal_relay_get(&bRelayOn)) stR.bRelayOn = bRelayOn;
    if (pstOut) *pstOut = stR;
    logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG, "execute id=%d ok=%d", (int)pstCmd->eId, (int)bOk);
    return bOk;
}
