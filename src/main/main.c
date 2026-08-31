/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "main.h"
#include "task_table.h"
#include "watchdog_table.h"

/* core */
#include "debug.h"
#include "data_store.h"
#include "power_mgmt.h"
#include "system.h"
/* network */
#include "wlan_com.h"
#include "task_com.h"
#include "local_server.h"
#include "network_config.h"
#include "link_quality.h"
/* device */
#include "hal.h"
#include "onewire_am2302.h"
#include "pump_task.h"
#include "command.h"
#include "task_measure.h"
#include "build_info_gen.h"   /* FOUNTAINER_BUILD_VERSION (pre-build generated) */
#include "factory_config.h"
/* components / ESP-IDF */
#include "datapoints.h"
#include "event_manager.h"
#include "logging.h"
#include "pressure_history.h"
#include "fountain_proto.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#define TAG "main"

/* WLAN provisioning now lives in the embedded network.json (real file is
 * git-ignored; see network_config.c) — it fills the Network_* datapoints on
 * first boot. This compile-time fallback is only the very last resort when
 * the store AND network.json are empty. */
#ifndef WIFI_SSID_FALLBACK
#define WIFI_SSID_FALLBACK ""
#endif
#ifndef WIFI_PASS_FALLBACK
#define WIFI_PASS_FALLBACK ""
#endif

/* =============================================================
 * Dependency-injection glue (STRATEGY/DI): the CORE modules
 * (power_mgmt, debug, system) know only these interfaces; main
 * wires them to the concrete network/device implementations.
 * ============================================================= */

static bool glue_pump_idle(void)
{
    return pump_idle_get();
}

static bool glue_console_up(void)
{
    bool bUsb = false;
    hal_usb_connected_get(&bUsb);
    return bUsb;
}

static void glue_pre_reboot(void)
{
    /* Safety first: pump relay OFF before ANY reboot path. */
    hal_relay_set(false);
    /* Clean WLAN teardown (protected deauth) -> fast reconnect after boot. */
    wlan_com_teardown_for_reboot(1000);
}

/* --- App-watchdog glue (channel IDs are main knowledge) ------------------- */

/* fp_task reports session progress ~1/s (negotiated & running). */
static void glue_wd_session_alive(void)
{
    wd_heartbeat(WD_CH_SESSION);
}

/* Runs in the event-manager dispatch task: proves the pipeline is alive. */
static void glue_wd_event_probe(system_event_t eEvent, const void *pvData,
                                size_t szSize)
{
    (void)eEvent; (void)pvData; (void)szSize;
    wd_heartbeat(WD_CH_EVENT);
}

/* Session established: heartbeat + release the reboot-loop brake. */
static void glue_wd_session_success(system_event_t eEvent, const void *pvData,
                                    size_t szSize)
{
    (void)eEvent; (void)pvData; (void)szSize;
    wd_heartbeat(WD_CH_SESSION);
    wd_session_success_note();
}

/* The interim diagnostic event subscriber from work package 1 is gone:
 * the logging bridge (package 3) writes the same events as STRUCTURED
 * records — the text twin only duplicated every event in the ring. */

/* Known-good network backup once a session is fully up (moved here from
 * task_com: subscribing decouples the network layer from network_config).
 * Runs in the dispatch task; the NVS write happens at most once per
 * connect and only on an actual config change (diff check inside). */
static void glue_session_backup(system_event_t eEvent, const void *pvData,
                                size_t szSize)
{
    (void)eEvent; (void)pvData; (void)szSize;
    /* A negotiated session is the trial-confirmation criterion (WLAN + IP +
     * TLS + auth all proven) — confirm first, then the backup below turns
     * the freshly confirmed values into the new known-good set. */
    network_config_trial_confirm();
    network_config_backup();
}

/* --- Communication-adaptation coordination (Link_Robustness_v1 §B2/B3) ----
 * TWO independent reasons want to stretch the protocol grid / control the
 * modem power save: the power manager (LOW mode) and a POOR link. They must
 * not fight over the same switches — main owns the combination:
 *   protocol slow grid = power LOW  OR  link POOR
 *   modem sleep        = power LOW  AND allowed by Net_PS_Override
 *                        (0=auto: a POOR link suspends sleeping,
 *                         1=never sleep, 2=always sleep in LOW). */
static volatile bool s_bPowerLow;
static volatile bool s_bLinkPoor;

static void comm_adaptation_apply(void)
{
    fountain_proto_slow_mode_set(s_bPowerLow || s_bLinkPoor);

    uint8_t ucOverride = DP_REF(Net_PS_Override);
    bool bSleep = s_bPowerLow;
    if (ucOverride == 1)                       bSleep = false;
    else if (ucOverride == 0 && s_bLinkPoor)   bSleep = false;
    wlan_com_ps_low_set(bSleep);
}

static void glue_power_low(bool bLow)          /* power_mgmt provider */
{
    s_bPowerLow = bLow;
    comm_adaptation_apply();
}

static void glue_proto_slow(bool bSlow)        /* power_mgmt provider */
{
    s_bPowerLow = bSlow;                       /* same source signal   */
    comm_adaptation_apply();
}

static void glue_link_state(system_event_t eEvent, const void *pvData,
                            size_t szSize)     /* EVT_LINK_STATE_CHANGED */
{
    (void)eEvent;
    if (pvData && szSize >= 1) {
        s_bLinkPoor = (*(const uint8_t *)pvData) != 0;
        comm_adaptation_apply();
    }
}

static void wire_core_hooks(void)
{
    system_pre_reboot_hook_set(glue_pre_reboot);
    debug_console_probe_set(glue_console_up);

    const power_mgmt_providers_t stProv = {
        .pump_idle    = glue_pump_idle,
        .session_up   = fountain_proto_running,
        .radio_ps_low = glue_power_low,        /* via comm coordination */
        .proto_slow   = glue_proto_slow,       /* via comm coordination */
    };
    power_mgmt_providers_set(&stProv);
}

/* =============================================================
 * Monitor task — the main module's supervision loop (point 3 of
 * Modularisation.md): gathers the SYSTEM metrics, drives the power
 * manager and watches the worker tasks.
 * ============================================================= */

/* CPU utilization in % over the last interval from the FreeRTOS runtime
 * statistics. Sums the idle tasks of both cores and compares the increment
 * with the total time increment (×2 cores). 0 on the first call. */
static uint8_t cpu_utilization_pct(void)
{
    static uint64_t s_ullPrevIdle = 0, s_ullPrevTotal = 0;
    UBaseType_t uxN = uxTaskGetNumberOfTasks();
    TaskStatus_t *pstArr = malloc(uxN * sizeof(TaskStatus_t));
    if (!pstArr) return 0;

    uint32_t ulTotalNow = 0;
    uxN = uxTaskGetSystemState(pstArr, uxN, &ulTotalNow);
    uint64_t ullIdle = 0;
    for (UBaseType_t i = 0; i < uxN; ++i)
        if (strncmp(pstArr[i].pcTaskName, "IDLE", 4) == 0)
            ullIdle += pstArr[i].ulRunTimeCounter;
    free(pstArr);

    uint64_t ullDTotal = (uint64_t)ulTotalNow - s_ullPrevTotal;
    uint64_t ullDIdle  = ullIdle - s_ullPrevIdle;
    s_ullPrevTotal = ulTotalNow;
    s_ullPrevIdle  = ullIdle;
    if (s_ullPrevTotal == ullDTotal || ullDTotal == 0) return 0;   /* first call */

    uint64_t ullCapacity = ullDTotal * 2;                /* two cores */
    if (ullDIdle > ullCapacity) ullDIdle = ullCapacity;
    return (uint8_t)(100u - (ullDIdle * 100u) / ullCapacity);
}

/* One monitor cycle (5 s): system metrics + power tick. The loop/timing and
 * the task supervision (cycle budget, stack watermark) are owned by the
 * task_manager component (TM_TASK_MONITOR in task_table.c). */
esp_err_t main_monitor_cycle(tm_task_ctx_t *pstCtx)
{
    (void)pstCtx;

    /* System metrics into the reportable datapoints. */
    DP_REF(System_Uptime)          = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    DP_REF(System_Min_Memory_Free) = (uint32_t)esp_get_minimum_free_heap_size();
    DP_REF(System_Reconnect_Count) = wlan_com_disconnect_count_get();
    DP_REF(System_Utilization)     = cpu_utilization_pct();
    DP_REF(System_Event_Drops)     = event_manager_drops_get();

    /* Smallest stack headroom (bytes) across the tasks managed by the
     * task_manager — data-structure/stack safety for continuous operation.
     * Covers the managed tasks (incl. the 200-ms pump task); local_server
     * workers/httpd are raw xTaskCreate and not captured here. */
    {
        uint32_t ulMinStack = UINT32_MAX;
        for (tm_task_id_t id = 0; id < TASK_MANAGER_MAX_TASKS; id++) {
            UBaseType_t uxWm = tm_get_stack_watermark(id);
            if (uxWm > 0 && (uint32_t)uxWm < ulMinStack) ulMinStack = (uint32_t)uxWm;
        }
        DP_REF(System_Min_Stack_Free) = (ulMinStack == UINT32_MAX) ? 0 : ulMinStack;
    }

    /* Low-memory watch (edge-triggered, re-arms with hysteresis). */
    {
        static bool s_bLowMemWarned = false;
        uint32_t ulFree = (uint32_t)esp_get_free_heap_size();
        if (ulFree < 40 * 1024 && !s_bLowMemWarned) {
            s_bLowMemWarned = true;
            event_manager_publish(EVT_LOW_MEMORY, &ulFree, sizeof(ulFree));
        } else if (ulFree > 48 * 1024) {
            s_bLowMemWarned = false;
        }
    }

    /* Logging: mirror stats into DPs + apply runtime config from DPs
     * (dp_write on Log_Enabled/Log_Runtime_Level becomes effective here). */
    {
        log_stats_t stLog;
        logging_stats_get(&stLog);
        DP_REF(Log_Next_Seq) = stLog.ulNextSeq;
        DP_REF(Log_Dropped)  = stLog.ulDropped;
        DP_REF(Log_Dropped_Flash) = logging_flash_dropped_get();
        uint32_t ulPrev = 0;
        DP_REF(Log_Prev_Boot_Available) =
            logging_previous_boot_available(&ulPrev) ? 1 : 0;
        logging_set_enabled(DP_REF(Log_Enabled) != 0);
        logging_set_runtime_level((log_level_t)DP_REF(Log_Runtime_Level));
        logging_set_flash_level((log_level_t)DP_REF(Log_Flash_Level));
    }

    /* Pressure history: mirror the ring stats for server diagnostics. */
    {
        pressure_history_stats_t stHist;
        pressure_history_stats_get(&stHist);
        DP_REF(Pressure_Hist_Next_Seq)    = stHist.next_seq;
        DP_REF(Pressure_Hist_Overwritten) = stHist.overwritten;
        DP_REF(Pressure_Hist_Highwater)   = stHist.high_watermark;
    }

    wifi_ap_record_t stAp;
    if (esp_wifi_sta_get_ap_info(&stAp) == ESP_OK)
        DP_REF(System_RSSI) = (int8_t)stAp.rssi;

    const esp_partition_t *pstNext = esp_ota_get_next_update_partition(NULL);
    if (pstNext) DP_REF(System_Flash_Free) = pstNext->size;

    /* Power management: LOW after 5 min without requests/active tasks. */
    power_mgmt_tick();

    /* Link supervision: score, Net_* datapoints, NET_SAMPLE records,
     * POOR/GOOD edges (Link_Robustness_v1 stage 1). */
    link_quality_tick();

    /* WLAN trial window (Network_Save=4): rollback + reboot on timeout. */
    network_config_trial_tick();

    /* Tick event: WD_EVENT liveness probe (subscriber heartbeats from the
     * dispatch task) + anyone interested in the 5-s cadence. */
    event_manager_publish(EVT_MONITOR_TICK, NULL, 0);
    return ESP_OK;
}

/* =============================================================
 * Init sequence
 * ============================================================= */

/* Resolve WLAN credentials in priority order: datapoints (NVS) -> legacy ESP
 * WLAN NVS (migrated once into the datapoints) -> compile-time fallback. */
static void wlan_credentials_resolve(const char **ppstrSsid, const char **ppstrPass)
{
    *ppstrSsid = DP_REF(Network_SSID);
    *ppstrPass = DP_REF(Network_Password);
    if ((*ppstrSsid)[0] != '\0') return;

    char astrLegacySsid[sizeof(DP_REF(Network_SSID))];
    char astrLegacyPass[sizeof(DP_REF(Network_Password))];
    if (!wlan_com_saved_credentials_get(astrLegacySsid, sizeof(astrLegacySsid),
                                        astrLegacyPass, sizeof(astrLegacyPass))) {
        *ppstrSsid = WIFI_SSID_FALLBACK;
        *ppstrPass = WIFI_PASS_FALLBACK;
        return;
    }

    strncpy(DP_REF(Network_SSID), astrLegacySsid, sizeof(DP_REF(Network_SSID)) - 1);
    DP_REF(Network_SSID)[sizeof(DP_REF(Network_SSID)) - 1] = '\0';
    strncpy(DP_REF(Network_Password), astrLegacyPass, sizeof(DP_REF(Network_Password)) - 1);
    DP_REF(Network_Password)[sizeof(DP_REF(Network_Password)) - 1] = '\0';

    if (dp_config_save() == ESP_OK)
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "WLAN credentials migrated from ESP WLAN NVS into datapoints");
    else
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "WLAN credentials migrated in RAM only (dp_config_save failed)");
}

/* Helper macro: check init, on error log a message + abort the sequence.
 * Feeds the TWDT per step: main_init is TWDT-subscribed (60 s, PANIC), so a
 * HANGING init step — busy or blocking — ends in panic -> reboot instead of
 * an invisible boot hang (the app watchdog is not running yet). */
#define INIT_STEP(call)                                       \
    do {                                                      \
        if (!(call)) {                                        \
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "ERROR at: %s", #call);   \
            return false;                                     \
        }                                                     \
        esp_task_wdt_reset();                                 \
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "ok: %s", #call);               \
    } while (0)

static bool main_init(void)
{
    /* Init supervision (work package 4): subscribe the main task to the
     * TWDT for the duration of the boot sequence. */
    esp_task_wdt_add(NULL);

    /* Logging core FIRST (boot_id, ring): every logging() call from here on
     * lands in the ring the server can pull via log_read. Then the RTC
     * reboot diagnosis (published once events/logging are up). */
    logging_init_early();
    pressure_history_init();
    wd_init();

    /* debug first, so that the following steps are logged. */
    if (!debug_init(DBG_LVL_VERBOSE)) return false;

    /* Data layer + hardware before the logic. */
    INIT_STEP(data_store_init());

    /* Factory identity (series production): serial, device_id, hw revision,
     * server, keys from the factory NVS partition; without it the compiled-in
     * legacy identity applies (same behaviour as before). task_com.c reads
     * the SAME factory_config, so datapoint and wire identity stay in sync
     * by construction (the U64 datapoint serializes as %016llX, spec §4.1). */
    INIT_STEP(factory_config_init() == ESP_OK);
    const factory_config_t *pstFc = factory_config_get();

    /* Datapoint store (configuration + network credentials, NVS-persistent).
     * dp_init() initializes NVS itself (idempotent). */
    dp_identity_t identity = {
        .serial     = pstFc->serial,
        .hw_version = pstFc->hw_rev,
        .sw_version = esp_app_get_description()->version,
        .build_ms   = FOUNTAINER_BUILD_VERSION,
    };
    dp_init(&identity);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "ok: dp_init");

    /* One greppable line for the production station (tools/verify_device.py). */
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "FACTORY-ID serial=%016llX device_id=%s hw=%s fw=%s build=%llu src=%s",
            (unsigned long long)pstFc->serial, pstFc->device_id, pstFc->hw_rev,
            esp_app_get_description()->version,
            (unsigned long long)FOUNTAINER_BUILD_VERSION,
            pstFc->from_factory ? "factory" : "builtin");

    /* Event manager early: subsequent module inits may subscribe/publish. */
    INIT_STEP(event_manager_init() == ESP_OK);
    INIT_STEP(event_manager_start() == ESP_OK);
    event_manager_subscribe(EVT_SESSION_READY, glue_session_backup);

    /* Logging bridges: selected events -> records; ESP-IDF W/E lines
     * (esp-tls, wifi, websocket) -> records. Config from the Log_* DPs. */
    logging_bridge_subscribe_events();
    logging_bridge_hook_esp_log();
    logging_set_enabled(DP_REF(Log_Enabled) != 0);
    logging_set_runtime_level((log_level_t)DP_REF(Log_Runtime_Level));
    logging_set_flash_level((log_level_t)DP_REF(Log_Flash_Level));
    /* Flash tier (logstore partition): claims this boot's slot and offers
     * the PREVIOUS boot's records; inert while the partition is absent. */
    logging_flash_init();

    /* Watchdog boot diagnosis: if the LAST reboot was watchdog-initiated,
     * publish + log it and mirror it into the diagnosis datapoints. */
    {
        wd_boot_diag_t stDiag;
        if (wd_boot_diag_get(&stDiag)) {
            DP_REF(System_WD_Last_Channel)    = stDiag.ucChannel;
            DP_REF(System_WD_Last_Checkpoint) = stDiag.usCheckpoint;
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "WATCHDOG REBOOT diagnosis: channel=%u checkpoint=%u "
                    "soft=%u uptime=%us (WD reboot %u without success)",
                    stDiag.ucChannel, stDiag.usCheckpoint, stDiag.ucSoftCount,
                    (unsigned)stDiag.ulUptimeS, stDiag.ucRebootsWithoutSuccess);
            LOG_EMIT2(LOG_LEVEL_ERROR, LOG_MOD_SYSTEM, LOG_EVT_WD_BOOT_DIAG,
                      stDiag.ucChannel, stDiag.usCheckpoint, "wd reboot diagnosis");
            evt_task_info_t stInfo = { .ucTaskId = stDiag.ucChannel,
                                       .slReason = stDiag.usCheckpoint };
            event_manager_publish(EVT_WD_BOOT_DIAGNOSIS, &stInfo, sizeof(stInfo));
        }
        DP_REF(System_WD_Reboot_Count) = stDiag.ucRebootsWithoutSuccess;
    }

    /* First boot / NVS schema change: seed the Network_* datapoints from the
     * embedded network.json (WLAN credentials, server address, DHCP/static). */
    network_config_defaults_apply();

    /* WLAN commit/confirm (Network_Save=4): on a trial boot this arms the
     * 120-s confirmation window; on an unconfirmed leftover (power loss)
     * it restores the known-good backup BEFORE the WLAN connect below. */
    network_config_trial_boot_check();

    /* Why did we boot? (1=power-on 3=software 4=panic 5/6/7=watchdog 9=brownout)
     * Fixed for this boot; reported via dp_report for remote diagnostics. */
    DP_REF(System_Reset_Reason) = (uint8_t)esp_reset_reason();
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "reset reason=%d",
            (int)DP_REF(System_Reset_Reason));
    {
        uint8_t ucReason = DP_REF(System_Reset_Reason);
        event_manager_publish(EVT_SYSTEM_BOOT, &ucReason, sizeof(ucReason));
    }

    INIT_STEP(hal_setup());
    INIT_STEP(am2302_init(HAL_DHT_GPIO));

    /* Domain logic: the pure pump manager + its I/O binding (config from
     * the Fon_* datapoints incl. the runtime-changeable sensor curve). */
    INIT_STEP(pump_task_init());
    INIT_STEP(command_init());

    /* Communication. The protocol (Fountain v2.2) resides in the component
     * clientside_protocol and is started from task_com on WLAN connect. */
    INIT_STEP(wlan_com_init());

    /* Link supervision (score/records/adaptation events); ticked by the
     * monitor. main coordinates the consumers (comm_adaptation_apply). */
    INIT_STEP(link_quality_init());
    event_manager_subscribe(EVT_LINK_STATE_CHANGED, glue_link_state);

    /* Wire the core hooks (DI) BEFORE anything can trigger them. */
    wire_core_hooks();

    /* Initiate WLAN connection — SSID/password from the datapoints (NVS),
     * otherwise compile-time fallback. Deliberately NOT fatal: without WLAN
     * measurement and local logs keep running. */
    const char *pstrSsid, *pstrPass;
    wlan_credentials_resolve(&pstrSsid, &pstrPass);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "WLAN-SSID: '%s'", pstrSsid);
    if (!wlan_com_connect(pstrSsid, pstrPass))
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "WLAN not started (missing/invalid credentials) — continuing without it");

    /* SNTP (non-blocking; syncs once the network is up). Correct wall-clock
     * time matters for the TLS certificate validity check; the testbed certs
     * are additionally backdated to 2020 so the first connect (before the
     * sync completes) does not fail with "certificate not yet valid". */
    esp_sntp_config_t stSntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    stSntp.start = true;
    if (esp_netif_sntp_init(&stSntp) == ESP_OK)
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "ok: sntp (pool.ntp.org)");

    /* Power management (DFS + modem sleep after idle) — the monitor task
     * drives its tick. Not fatal: without it the device stays at full clock. */
    power_mgmt_init();

    /* Cyclic app tasks run under the task_manager component (registration
     * from the project table); the protocol starter stays independent. */
    INIT_STEP(tm_init() == ESP_OK);
    INIT_STEP(task_table_register_all() == ESP_OK);
    INIT_STEP(task_com_start());
    /* Local WSS maintenance server (firmware_server.md): starts/stops with
     * the WLAN events; needs task_com's TLS getters (factory cert). */
    INIT_STEP(local_server_init() == ESP_OK);
    INIT_STEP(tm_start_all() == ESP_OK);

    /* App watchdog LAST (all heartbeat sources are running now): channels
     * from the project table, session progress via the injected hook, the
     * event pipeline via the EVT_MONITOR_TICK subscriber. */
    INIT_STEP(watchdog_table_register_all() == ESP_OK);
    task_com_alive_hook_set(glue_wd_session_alive);
    event_manager_subscribe(EVT_MONITOR_TICK, glue_wd_event_probe);
    event_manager_subscribe(EVT_SESSION_READY, glue_wd_session_success);
    INIT_STEP(wd_start() == ESP_OK);

    event_manager_publish(EVT_SYSTEM_READY, NULL, 0);
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "initialization complete");

    /* Boot sequence done — the main task leaves TWDT supervision (the app
     * tasks and the app watchdog carry it from here). */
    esp_task_wdt_delete(NULL);
    return true;
}

/* ESP-IDF entry point. */
void app_main(void)
{
    if (!main_init()) {
        /* Fail safe: relay off, then reboot into a fresh attempt. The event
         * lands in the log ring (bridge) and thus in the NEXT boot's
         * diagnosis once the flash tier exists. */
        hal_relay_set(false);
        event_manager_publish(EVT_FATAL_ERROR, NULL, 0);
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "init FAILED — relay off, reboot in 10 s");
        system_reboot_deferred(10000);
    }
    /* app_main may return; the work continues in the tasks. */
}
