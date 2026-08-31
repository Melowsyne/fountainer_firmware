/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "watchdog_table.h"

#include "system.h"
#include "wlan_com.h"
#include "fountain_proto.h"

/* Adapters: the component wants (void*)-style probes/actions. */

static bool wd_glue_reboot(uint32_t ulDelayMs)
{
    return system_reboot_deferred(ulDelayMs);   /* core path incl. pre-reboot hook */
}

static bool wd_glue_session_recover(void *pvArg)
{
    (void)pvArg;
    return fountain_proto_recover();            /* hard WS-client restart */
}

static bool wd_glue_link_up(void *pvArg)
{
    (void)pvArg;
    return wlan_com_connected_get();            /* no link -> reboot cannot help */
}

static const struct { uint32_t ulId; wd_channel_def_t stDef; } s_astChannels[] = {
    { WD_CH_SESSION, { .name = "session", .deadline_ms = 180000, .max_soft = 5,
                       .recover = wd_glue_session_recover,
                       .reboot_allowed = wd_glue_link_up,
                       .escalate_reboot = true } },
    { WD_CH_MEASURE, { .name = "measure", .deadline_ms = 15000, .max_soft = 1,
                       .escalate_reboot = true } },
    { WD_CH_MONITOR, { .name = "monitor", .deadline_ms = 15000, .max_soft = 1,
                       .escalate_reboot = true } },
    { WD_CH_EVENT,   { .name = "event",   .deadline_ms = 30000, .max_soft = 1,
                       .escalate_reboot = true } },
    /* Pump control is the safety-critical path: a stalled 200-ms cycle gets
     * ONE grace period, then the device reboots (pre-reboot hook forces the
     * relay OFF; the TWDT panic at 60 s remains the hard fallback). */
    { WD_CH_PUMP,    { .name = "pump",    .deadline_ms = 2000,  .max_soft = 1,
                       .escalate_reboot = true } },
};

esp_err_t watchdog_table_register_all(void)
{
    wd_reboot_hook_set(wd_glue_reboot);
    for (size_t i = 0; i < sizeof(s_astChannels) / sizeof(s_astChannels[0]); i++) {
        esp_err_t err = wd_register(s_astChannels[i].ulId, &s_astChannels[i].stDef);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
