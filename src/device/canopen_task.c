/* Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved. */
/* canopen_task.c — see canopen_task.h. Structure:
 *   1. state, time, logging helpers
 *   2. object dictionary glue (datapoints <-> CANopen)
 *   3. TWAI driver (install/start/stop, timing presets)
 *   4. core start (identity, default PDO set) + event glue (EMCY)
 *   5. canopen_init / cycle / recover
 */
#include "canopen_task.h"
#include "co_core.h"
#include "hal.h"
#include "datapoints.h"
#include "factory_config.h"
#include "logging.h"
#include "debug.h"
#include "event_manager.h"
#include "system_events.h"
#include "task_com.h"
#include "driver/twai.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "canopen"

/* Can_State encoding (dp_list.def) */
enum { CAN_ST_OFF = 0, CAN_ST_INIT, CAN_ST_PREOP, CAN_ST_OPERATIONAL,
       CAN_ST_STOPPED, CAN_ST_BUS_OFF };

#define CO_IDX_DP_BASE     0x2000u   /* datapoint i -> 0x2000 + i           */
#define CO_IDX_DP_COUNT    0x2FFFu   /* U16: DP_COUNT                       */
#define CO_IDX_DESC_BASE   0x3000u   /* descriptor record for datapoint i   */
#define CO_PRODUCT_CODE    0x464E5400u /* 'F','N','T',0                    */
#define CO_BUS_OFF_STALL_MS 3000u    /* bus-off longer than this -> WD recover */
/* Unconnected/unterminated bus: every own frame ends in bus-off (no ACK).
 * After CO_ABSENT_BUS_OFFS consecutive bus-offs without a single received
 * frame the driver pauses for CO_ABSENT_HOLD_MS ("bus absent") instead of
 * cycling bus-off once per heartbeat — keeps the log and the transceiver
 * quiet, retries automatically. */
#define CO_ABSENT_BUS_OFFS  5u
#define CO_ABSENT_HOLD_MS   30000u
#define CO_RX_BURST_MAX    32        /* frames drained per 10-ms cycle      */

/* ---------------------------------------------------------------------------
 * 1. State
 * ------------------------------------------------------------------------- */
static struct {
    bool       bHw;              /* transceiver on this board              */
    bool       bRunning;         /* TWAI driver installed + started        */
    co_node_t  node;
    /* active configuration (mirrors the Can_* NVS points) */
    bool       bEnabled;
    bool       bLoopback;        /* TWAI_MODE_NO_ACK self-test (Can_Loopback) */
    uint8_t    ucNodeId;
    uint16_t   usKbps;
    uint16_t   usHeartbeatMs;
    uint32_t   ulLastHousekeepingMs;
    /* bus state */
    bool       bBusOff;
    uint32_t   ulBusOffSinceMs;
    uint16_t   usBusOffCount;
    uint32_t   ulBusErrorsAccum;  /* counters of previous driver instances */
    uint32_t   ulRxAccum, ulTxAccum;
    uint32_t   ulSdoAccum, ulPdoTxAccum, ulPdoRxAccum, ulEmcyAccum;
    bool       bMasterActive;
    uint8_t    ucBusOffStreak;    /* consecutive bus-offs without any RX     */
    uint32_t   ulRxSeen;          /* rx_frames at the last bus-off           */
    bool       bAbsent;           /* driver paused, waiting for CO_ABSENT_HOLD_MS */
    uint32_t   ulAbsentSinceMs;
    /* pump fault -> EMCY (set from the event task, consumed in the cycle) */
    volatile uint8_t ucFaultPending;   /* 0 none, 1 fault, 2 cleared       */
    volatile uint8_t ucFaultCode;
} s;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------------------
 * 2. Object dictionary glue
 * ------------------------------------------------------------------------- */
/* Secrets never leave the device over the (unauthenticated) CAN bus. */
static bool dp_is_secret(const dp_desc_t *d)
{
    return strcmp(d->name, "Network_Password") == 0 ||
           strcmp(d->name, "Backup_Password") == 0;
}

static uint32_t od_read_dp(const dp_desc_t *d, uint8_t *buf, size_t cap, size_t *len)
{
    if (d->access == DP_WO || dp_is_secret(d)) return CO_ABORT_WRITE_ONLY;
    uint8_t raw[DP_STR_MAX];
    size_t  n = dp_type_size[d->type];
    if (n > sizeof raw) return CO_ABORT_GENERAL;
    dp_lock(portMAX_DELAY);
    memcpy(raw, DP_SLOT(d), n);
    dp_unlock();
    if (d->type == DP_STR) n = strnlen((const char *)raw, DP_STR_MAX);
    if (n > cap) return CO_ABORT_OUT_OF_MEMORY;
    memcpy(buf, raw, n);
    *len = n;
    return 0;
}

static uint32_t od_read_cb(void *arg, uint16_t index, uint8_t sub,
                           uint8_t *buf, size_t cap, size_t *len)
{
    (void)arg;
    if (index == CO_IDX_DP_COUNT) {
        if (sub) return CO_ABORT_NO_SUBINDEX;
        if (cap < 2) return CO_ABORT_OUT_OF_MEMORY;
        co_put_u16(buf, (uint16_t)DP_COUNT); *len = 2; return 0;
    }
    if (index >= CO_IDX_DP_BASE && index < CO_IDX_DP_BASE + DP_COUNT) {
        if (sub) return CO_ABORT_NO_SUBINDEX;
        return od_read_dp(&g_dp[index - CO_IDX_DP_BASE], buf, cap, len);
    }
    if (index >= CO_IDX_DESC_BASE && index < CO_IDX_DESC_BASE + DP_COUNT) {
        const dp_desc_t *d = &g_dp[index - CO_IDX_DESC_BASE];
        switch (sub) {
        case 0: if (cap < 1) return CO_ABORT_OUT_OF_MEMORY; buf[0] = 3; *len = 1; return 0;
        case 1: { size_t n = strlen(d->name);
                  if (n > cap) return CO_ABORT_OUT_OF_MEMORY;
                  memcpy(buf, d->name, n); *len = n; return 0; }
        case 2: if (cap < 1) return CO_ABORT_OUT_OF_MEMORY; buf[0] = d->type;   *len = 1; return 0;
        case 3: if (cap < 1) return CO_ABORT_OUT_OF_MEMORY;
                buf[0] = dp_is_secret(d) ? (uint8_t)DP_WO : d->access; *len = 1; return 0;
        default: return CO_ABORT_NO_SUBINDEX;
        }
    }
    return CO_ABORT_NO_OBJECT;
}

static uint32_t abort_from_dp_error(const char *code)
{
    if (!code) return CO_ABORT_GENERAL;
    if (strcmp(code, "out_of_range") == 0)         return CO_ABORT_VALUE_RANGE;
    if (strcmp(code, "type_mismatch") == 0)        return CO_ABORT_TYPE_MISMATCH;
    if (strcmp(code, "read_only") == 0)            return CO_ABORT_READ_ONLY;
    if (strcmp(code, "unknown_name") == 0)         return CO_ABORT_NO_OBJECT;
    if (strcmp(code, "constraint_violation") == 0) return CO_ABORT_PARAM_INCOMPAT;
    if (strcmp(code, "invalid_network_save") == 0) return CO_ABORT_VALUE_RANGE;
    return CO_ABORT_GENERAL;
}

/* Build the JSON value for the datapoint from CANopen little-endian bytes. */
static cJSON *json_from_raw(const dp_desc_t *d, const uint8_t *buf, size_t len,
                            uint32_t *abort)
{
    size_t need = dp_type_size[d->type];
    if (d->type == DP_STR) {
        if (len >= DP_STR_MAX) { *abort = CO_ABORT_TYPE_TOO_HIGH; return NULL; }
        char tmp[DP_STR_MAX];
        memcpy(tmp, buf, len); tmp[len] = '\0';
        return cJSON_CreateString(tmp);
    }
    if (len != need) {
        *abort = len > need ? CO_ABORT_TYPE_TOO_HIGH : CO_ABORT_TYPE_TOO_LOW;
        return NULL;
    }
    switch (d->type) {
    case DP_BOOL: return cJSON_CreateBool(buf[0] != 0);
    case DP_U8: case DP_ENUM: return cJSON_CreateNumber(buf[0]);
    case DP_I8:   return cJSON_CreateNumber((int8_t)buf[0]);
    case DP_U16:  return cJSON_CreateNumber(co_get_u16(buf));
    case DP_I16:  return cJSON_CreateNumber((int16_t)co_get_u16(buf));
    case DP_U32:  return cJSON_CreateNumber((double)co_get_u32(buf));
    case DP_I32:  return cJSON_CreateNumber((int32_t)co_get_u32(buf));
    case DP_F32:  { float f; memcpy(&f, buf, 4); return cJSON_CreateNumber(f); }
    case DP_U64:  { uint64_t v; memcpy(&v, buf, 8); char sbuf[19];
                    snprintf(sbuf, sizeof sbuf, "0x%016llX", (unsigned long long)v);
                    return cJSON_CreateString(sbuf); }
    default: *abort = CO_ABORT_TYPE_MISMATCH; return NULL;
    }
}

static uint32_t od_write_cb(void *arg, uint16_t index, uint8_t sub,
                            const uint8_t *buf, size_t len)
{
    (void)arg;
    if (index == CO_IDX_DP_COUNT ||
        (index >= CO_IDX_DESC_BASE && index < CO_IDX_DESC_BASE + DP_COUNT))
        return CO_ABORT_READ_ONLY;
    if (index < CO_IDX_DP_BASE || index >= CO_IDX_DP_BASE + DP_COUNT)
        return CO_ABORT_NO_OBJECT;
    if (sub) return CO_ABORT_NO_SUBINDEX;

    const dp_desc_t *d = &g_dp[index - CO_IDX_DP_BASE];
    if (d->access == DP_RO) return CO_ABORT_READ_ONLY;

    uint32_t abort = 0;
    cJSON *pVal = json_from_raw(d, buf, len, &abort);
    if (!pVal) return abort ? abort : CO_ABORT_OUT_OF_MEMORY;

    cJSON *pDp  = cJSON_CreateObject();
    cJSON *pRes = cJSON_CreateObject();
    if (!pDp || !pRes) {
        cJSON_Delete(pVal); cJSON_Delete(pDp); cJSON_Delete(pRes);
        return CO_ABORT_OUT_OF_MEMORY;
    }
    cJSON_AddItemToObject(pDp, d->name, pVal);
    /* Same path as cloud/local dp_write: validation, constraints, NVS,
     * Network_Save/Log_Command interception, EVT_DP_WRITTEN. */
    task_com_apply_dp_write(pDp, pRes);

    const cJSON *pStatus = cJSON_GetObjectItemCaseSensitive(pRes, "status");
    if (!cJSON_IsString(pStatus) || strcmp(pStatus->valuestring, "applied") != 0) {
        /* errors{} is keyed by the culprit's name — for a cross-field
         * constraint that may be the OTHER field (e.g. Fon_Max_Pressure when
         * Fon_Min_Pressure was written), so fall back to the first entry. */
        const cJSON *pErrs = cJSON_GetObjectItemCaseSensitive(pRes, "errors");
        const cJSON *pErr  = pErrs ? cJSON_GetObjectItemCaseSensitive(pErrs, d->name) : NULL;
        if (!cJSON_IsString(pErr) && pErrs && cJSON_IsObject(pErrs)) pErr = pErrs->child;
        if (!cJSON_IsString(pErr)) pErr = cJSON_GetObjectItemCaseSensitive(pRes, "error");
        abort = abort_from_dp_error(cJSON_IsString(pErr) ? pErr->valuestring : NULL);
    }
    cJSON_Delete(pDp);
    cJSON_Delete(pRes);

    LOG_EMIT4(abort ? LOG_LEVEL_WARN : LOG_LEVEL_INFO, LOG_MOD_CANOPEN,
              LOG_EVT_CAN_SDO_WRITE, (int32_t)index, (int32_t)sub, (int32_t)abort,
              (int32_t)len, d->name);
    return abort;
}

/* ---------------------------------------------------------------------------
 * 3. TWAI driver
 * ------------------------------------------------------------------------- */
static bool timing_for(uint16_t usKbps, twai_timing_config_t *pstOut)
{
    switch (usKbps) {
    case 10:   { twai_timing_config_t t = TWAI_TIMING_CONFIG_10KBITS();  *pstOut = t; return true; }
    case 20:   { twai_timing_config_t t = TWAI_TIMING_CONFIG_20KBITS();  *pstOut = t; return true; }
    case 50:   { twai_timing_config_t t = TWAI_TIMING_CONFIG_50KBITS();  *pstOut = t; return true; }
    case 100:  { twai_timing_config_t t = TWAI_TIMING_CONFIG_100KBITS(); *pstOut = t; return true; }
    case 125:  { twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS(); *pstOut = t; return true; }
    case 250:  { twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS(); *pstOut = t; return true; }
    case 500:  { twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS(); *pstOut = t; return true; }
    case 800:  { twai_timing_config_t t = TWAI_TIMING_CONFIG_800KBITS(); *pstOut = t; return true; }
    case 1000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();   *pstOut = t; return true; }
    default: return false;
    }
}

static bool driver_start(void)
{
    const hal_pins_t *p = hal_pins();
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        p->eCanTx, p->eCanRx, s.bLoopback ? TWAI_MODE_NO_ACK : TWAI_MODE_NORMAL);
    g.tx_queue_len   = 16;
    g.rx_queue_len   = 32;
    g.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                       TWAI_ALERT_ERR_PASS | TWAI_ALERT_ERR_ACTIVE |
                       TWAI_ALERT_RX_QUEUE_FULL;
    twai_timing_config_t t;
    if (!timing_for(s.usKbps, &t)) {
        LOG_EMIT2(LOG_LEVEL_ERROR, LOG_MOD_CANOPEN, LOG_EVT_CAN_DRIVER_ERROR,
                  (int32_t)s.usKbps, 0, "unsupported bitrate");
        return false;
    }
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    esp_err_t e = twai_driver_install(&g, &t, &f);
    if (e != ESP_OK) {
        LOG_EMIT2(LOG_LEVEL_ERROR, LOG_MOD_CANOPEN, LOG_EVT_CAN_DRIVER_ERROR,
                  (int32_t)e, 1, "twai_driver_install");
        return false;
    }
    e = twai_start();
    if (e != ESP_OK) {
        LOG_EMIT2(LOG_LEVEL_ERROR, LOG_MOD_CANOPEN, LOG_EVT_CAN_DRIVER_ERROR,
                  (int32_t)e, 2, "twai_start");
        twai_driver_uninstall();
        return false;
    }
    hal_can_standby_set(false);               /* transceiver -> normal mode */
    s.bBusOff  = false;
    s.bRunning = true;
    return true;
}

static void driver_stop(void)
{
    if (!s.bRunning) return;
    hal_can_standby_set(true);
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK)
        s.ulBusErrorsAccum += st.bus_error_count + st.tx_failed_count;
    s.ulBusErrorsAccum += s.node.stats.tx_failed;
    s.ulRxAccum    += s.node.stats.rx_frames;
    s.ulTxAccum    += s.node.stats.tx_frames;
    s.ulSdoAccum   += s.node.stats.sdo_requests;
    s.ulPdoTxAccum += s.node.stats.pdo_tx;
    s.ulPdoRxAccum += s.node.stats.pdo_rx;
    s.ulEmcyAccum  += s.node.stats.emcy_tx;
    memset(&s.node.stats, 0, sizeof s.node.stats);
    twai_stop();
    twai_driver_uninstall();
    s.bRunning = false;
    s.bBusOff  = false;
}

static bool can_send_cb(void *arg, const co_frame_t *f)
{
    (void)arg;
    if (!s.bRunning || s.bBusOff) return false;
    twai_message_t m;
    memset(&m, 0, sizeof m);
    m.identifier       = f->id;
    m.data_length_code = f->dlc;
    m.rtr              = f->rtr;
    memcpy(m.data, f->data, sizeof m.data);
    return twai_transmit(&m, pdMS_TO_TICKS(20)) == ESP_OK;
}

/* ---------------------------------------------------------------------------
 * 4. Core start + event glue
 * ------------------------------------------------------------------------- */
#define MAP_DP(dp, bits) ((((uint32_t)CO_IDX_DP_BASE + (uint32_t)DP_ID_##dp) << 16) | (uint32_t)(bits))

/* Default process data (DOKU/CANopen.md §PDO). A master may remap via
 * 0x1800../0x1A00.. — the defaults return on every reset communication. */
static void pdo_defaults(void)
{
    static const uint32_t t1[] = { MAP_DP(Fon_Current_Pressure, 32), MAP_DP(Fon_Current_State, 8),
                                   MAP_DP(Fon_Relay_Output, 8),      MAP_DP(Fon_Fault_Code, 8) };
    static const uint32_t t2[] = { MAP_DP(Fon_Pressure_Filtered, 32), MAP_DP(Fon_Pressure_Slope, 32) };
    static const uint32_t t3[] = { MAP_DP(System_Uptime, 32), MAP_DP(Fon_Run_Time, 32) };
    static const uint32_t t4[] = { MAP_DP(System_Temperature, 32), MAP_DP(Net_Link_Score, 8),
                                   MAP_DP(Fon_Demand_State, 8),     MAP_DP(Fon_Starts_Per_Hour, 8),
                                   MAP_DP(System_Power_Mode, 8) };
    static const uint32_t r1[] = { MAP_DP(Fon_Fault_Ack, 8), MAP_DP(Fon_Event_Label, 8) };
    bool ok = true;
    ok &= co_tpdo_configure(&s.node, 0, 0, 255,  200, 1000, t1, 4);
    ok &= co_tpdo_configure(&s.node, 1, 0, 255,  500, 2000, t2, 2);
    ok &= co_tpdo_configure(&s.node, 2, 0, 255, 1000, 5000, t3, 2);
    ok &= co_tpdo_configure(&s.node, 3, 0, 255, 1000, 5000, t4, 5);
    ok &= co_rpdo_configure(&s.node, 0, 0, 255, r1, 2);
    if (!ok)
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "default PDO mapping rejected (catalog changed?)");
}

static void cb_reset(void *arg, bool bResetNode)
{
    (void)arg;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "NMT reset %s",
            bResetNode ? "node" : "communication");
    pdo_defaults();
    /* Re-arm the error register from the live pump state. */
    if (DP_REF(Fon_Fault_Code) != 0)
        s.node.error_register |= CO_ERR_GENERIC | CO_ERR_DEVICE_SPECIFIC;
}

static void cb_nmt_changed(void *arg, co_nmt_state_t eOld, co_nmt_state_t eNew)
{
    (void)arg;
    LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_CANOPEN, LOG_EVT_CAN_NMT,
              (int32_t)eOld, (int32_t)eNew, "nmt state");
}

static uint32_t sw_revision_u32(const char *pstrVer)
{
    unsigned a = 0, b = 0, c = 0;
    if (pstrVer) sscanf(pstrVer, "%u.%u.%u", &a, &b, &c);
    return ((a & 0xFFu) << 16) | ((b & 0xFFu) << 8) | (c & 0xFFu);
}

static void core_start(void)
{
    co_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.node_id      = s.ucNodeId;
    cfg.heartbeat_ms = s.usHeartbeatMs;
    cfg.device_type  = 0;                          /* no CiA device profile */
    cfg.vendor_id    = 0;                          /* no registered CiA vendor id */
    cfg.product_code = CO_PRODUCT_CODE;
    cfg.revision     = sw_revision_u32(DP_REF(Device_SW_Version));
    cfg.serial       = (uint32_t)DP_REF(Device_Serial_Number);
    cfg.device_name  = "Fountainer";
    cfg.hw_version   = factory_config_get()->hw_rev;
    cfg.sw_version   = DP_REF(Device_SW_Version);
    cfg.send         = can_send_cb;
    cfg.od_read      = od_read_cb;
    cfg.od_write     = od_write_cb;
    cfg.on_reset     = cb_reset;
    cfg.on_nmt_changed = cb_nmt_changed;
    co_init(&s.node, &cfg, now_ms());
    pdo_defaults();
    if (DP_REF(Fon_Fault_Code) != 0)
        s.node.error_register |= CO_ERR_GENERIC | CO_ERR_DEVICE_SPECIFIC;
}

static void evt_pump_fault(system_event_t eEvent, const void *pvData, size_t szSize)
{
    if (eEvent == EVT_PUMP_FAULT) {
        s.ucFaultCode    = (pvData && szSize >= 1) ? *(const uint8_t *)pvData : 0;
        s.ucFaultPending = 1;
    } else {
        s.ucFaultPending = 2;
    }
}

static bool start_all(void)
{
    if (!driver_start()) return false;
    core_start();
    LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_CANOPEN, LOG_EVT_CAN_STARTED,
              (int32_t)s.ucNodeId, (int32_t)s.usKbps,
              s.bLoopback ? "canopen started (NO-ACK self-test)" : "canopen started");
    return true;
}

static void stop_all(int32_t slReason)
{
    if (!s.bRunning) return;
    driver_stop();
    LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_CANOPEN, LOG_EVT_CAN_STOPPED, slReason, 0, "canopen stopped");
}

static void config_snapshot(void)
{
    s.bEnabled      = DP_REF(Can_Enabled) != 0;
    s.bLoopback     = DP_REF(Can_Loopback) != 0;
    s.ucNodeId      = DP_REF(Can_Node_Id);
    s.usKbps        = DP_REF(Can_Bitrate);
    s.usHeartbeatMs = DP_REF(Can_Heartbeat_Ms);
}

static uint8_t state_code(void)
{
    if (!s.bHw) return CAN_ST_OFF;
    if (s.bAbsent) return CAN_ST_BUS_OFF;
    if (!s.bRunning) return CAN_ST_OFF;
    if (s.bBusOff) return CAN_ST_BUS_OFF;
    switch (co_nmt_state(&s.node)) {
    case CO_NMT_INITIALISING:   return CAN_ST_INIT;
    case CO_NMT_PREOPERATIONAL: return CAN_ST_PREOP;
    case CO_NMT_OPERATIONAL:    return CAN_ST_OPERATIONAL;
    case CO_NMT_STOPPED:        return CAN_ST_STOPPED;
    }
    return CAN_ST_INIT;
}

/* Master presence (Can_Master_Active / _Age_S): a frame addressed to this
 * node within Can_Master_Timeout_Ms. Edge-logged. */
static void master_presence_update(uint32_t ulNow)
{
    bool bActive = s.bRunning &&
                   co_master_active(&s.node, ulNow, DP_REF(Can_Master_Timeout_Ms));
    if (bActive != s.bMasterActive) {
        s.bMasterActive = bActive;
        LOG_EMIT2(bActive ? LOG_LEVEL_INFO : LOG_LEVEL_WARN, LOG_MOD_CANOPEN,
                  LOG_EVT_CAN_MASTER, bActive ? 1 : 0, 0,
                  bActive ? "CAN master active" : "CAN master lost");
    }
    DP_REF(Can_Master_Active) = bActive ? 1 : 0;
    DP_REF(Can_Master_Age_S)  = (s.bRunning && s.node.master_seen)
                                    ? (ulNow - s.node.last_master_ms) / 1000u
                                    : 0xFFFFFFFFu;
}

/* Once per second: Can_* mirrors, computed inputs, config re-apply. */
static void housekeeping(uint32_t ulNow)
{
    s.ulLastHousekeepingMs = ulNow;
    dp_refresh();                              /* heap figures for SDO reads */

    DP_REF(Can_State)     = state_code();
    DP_REF(Can_Rx_Frames) = s.ulRxAccum + s.node.stats.rx_frames;
    DP_REF(Can_Tx_Frames) = s.ulTxAccum + s.node.stats.tx_frames;
    uint32_t ulErr = s.ulBusErrorsAccum + s.node.stats.tx_failed;
    twai_status_info_t st;
    if (s.bRunning && twai_get_status_info(&st) == ESP_OK)
        ulErr += st.bus_error_count + st.tx_failed_count;
    DP_REF(Can_Bus_Errors)    = ulErr;
    DP_REF(Can_Bus_Off_Count) = s.usBusOffCount;
    DP_REF(Can_Sdo_Count)     = s.ulSdoAccum   + s.node.stats.sdo_requests;
    DP_REF(Can_Pdo_Tx_Count)  = s.ulPdoTxAccum + s.node.stats.pdo_tx;
    DP_REF(Can_Pdo_Rx_Count)  = s.ulPdoRxAccum + s.node.stats.pdo_rx;
    DP_REF(Can_Emcy_Count)    = (uint16_t)(s.ulEmcyAccum + s.node.stats.emcy_tx);
    DP_REF(Can_Nmt_State)     = s.bRunning ? (uint8_t)co_nmt_state(&s.node) : 0;
    master_presence_update(ulNow);

    bool     bEn  = DP_REF(Can_Enabled) != 0;
    bool     bLb  = DP_REF(Can_Loopback) != 0;
    uint8_t  ucId = DP_REF(Can_Node_Id);
    uint16_t usKb = DP_REF(Can_Bitrate);
    uint16_t usHb = DP_REF(Can_Heartbeat_Ms);
    if (!s.bRunning) {
        if (s.bAbsent && ulNow - s.ulAbsentSinceMs < CO_ABSENT_HOLD_MS) return;
        if (bEn) {
            config_snapshot();
            if (start_all()) {
                if (s.bAbsent) LOG_EMIT0(LOG_LEVEL_DEBUG, LOG_MOD_CANOPEN, LOG_EVT_CAN_STARTED,
                                         "retry after bus absent");
                s.bAbsent = false;
            }
        }
    } else if (!bEn) {
        stop_all(0);
        s.bEnabled = false;
    } else if (ucId != s.ucNodeId || usKb != s.usKbps || bLb != s.bLoopback) {
        stop_all(1);
        config_snapshot();
        (void)start_all();
    } else if (usHb != s.usHeartbeatMs) {
        s.usHeartbeatMs = usHb;
        co_heartbeat_set(&s.node, usHb);
    }
}

/* ---------------------------------------------------------------------------
 * 5. Public entry points
 * ------------------------------------------------------------------------- */
bool canopen_init(void)
{
    memset(&s, 0, sizeof s);
    s.bHw = hal_can_available();
    if (!s.bHw) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "no CAN transceiver on this board — idle");
        DP_REF(Can_State) = CAN_ST_OFF;
        return true;
    }
    event_manager_subscribe(EVT_PUMP_FAULT, evt_pump_fault);
    event_manager_subscribe(EVT_PUMP_FAULT_CLEARED, evt_pump_fault);
    config_snapshot();
    if (!s.bEnabled) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "disabled (Can_Enabled=0)");
        return true;
    }
    if (!start_all())
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "driver start failed — retrying from the cycle");
    else
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "node %u @ %u kbit/s, heartbeat %u ms (tx IO%d rx IO%d)",
                s.ucNodeId, s.usKbps, s.usHeartbeatMs, hal_pins()->eCanTx, hal_pins()->eCanRx);
    DP_REF(Can_State) = state_code();
    return true;
}

esp_err_t canopen_task_cycle(tm_task_ctx_t *pstCtx)
{
    (void)pstCtx;
    if (!s.bHw) return ESP_OK;
    uint32_t ulNow = now_ms();

    if (ulNow - s.ulLastHousekeepingMs >= 1000) housekeeping(ulNow);
    if (!s.bRunning) return ESP_OK;

    /* Bus state via alerts (non-blocking). */
    uint32_t ulAlerts = 0;
    if (twai_read_alerts(&ulAlerts, 0) == ESP_OK && ulAlerts) {
        if (ulAlerts & TWAI_ALERT_BUS_OFF) {
            s.bBusOff = true;
            s.ulBusOffSinceMs = ulNow;
            s.usBusOffCount++;
            if (s.node.stats.rx_frames == s.ulRxSeen) s.ucBusOffStreak++;
            else                                       s.ucBusOffStreak = 1;
            s.ulRxSeen = s.node.stats.rx_frames;
            if (s.ucBusOffStreak >= CO_ABSENT_BUS_OFFS) {
                /* Nobody acknowledges: bus absent. Pause instead of looping. */
                LOG_EMIT2(LOG_LEVEL_WARN, LOG_MOD_CANOPEN, LOG_EVT_CAN_BUS_OFF,
                          (int32_t)s.usBusOffCount, 1,
                          "no partner on the bus — pausing 30 s");
                stop_all(3);
                s.bAbsent = true;
                s.ulAbsentSinceMs = ulNow;
                s.ucBusOffStreak = 0;
                DP_REF(Can_State) = CAN_ST_BUS_OFF;
                return ESP_OK;
            }
            LOG_EMIT2(s.ucBusOffStreak == 1 ? LOG_LEVEL_WARN : LOG_LEVEL_DEBUG,
                      LOG_MOD_CANOPEN, LOG_EVT_CAN_BUS_OFF,
                      (int32_t)s.usBusOffCount, 0, "bus-off, recovering");
            twai_initiate_recovery();
        }
        if (ulAlerts & TWAI_ALERT_BUS_RECOVERED) {
            /* Recovery leaves the controller stopped — restart it. */
            if (twai_start() == ESP_OK) {
                s.bBusOff = false;
                LOG_EMIT0(LOG_LEVEL_INFO, LOG_MOD_CANOPEN, LOG_EVT_CAN_RECOVERED, "bus recovered");
                for (uint8_t i = 0; i < CO_TPDO_COUNT; i++) co_tpdo_request(&s.node, i);
            }
        }
        if (ulAlerts & TWAI_ALERT_RX_QUEUE_FULL)
            logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG, "rx queue full — frames lost");
    }
    if (s.bBusOff) {
        /* Stalled bus-off: stop heartbeating the watchdog channel so that
         * canopen_recover() reinstalls the driver after the 5-s deadline. */
        return (ulNow - s.ulBusOffSinceMs > CO_BUS_OFF_STALL_MS) ? ESP_FAIL : ESP_OK;
    }

    /* Event-driven RX for one task period: block in twai_receive (1 tick,
     * wakes immediately on a frame), process, tick the core, repeat until
     * the period has elapsed. SDO/RPDO latency is then bounded by the
     * processing time instead of the 10-ms polling grid; the task never
     * busy-waits (FreeRTOS tick = 10 ms, so the wait is at least 1 tick). */
    const uint32_t ulStart = ulNow;
    do {
        twai_message_t m;
        bool bHave = twai_receive(&m, 1) == ESP_OK;
        ulNow = now_ms();
        for (int i = 0; i < CO_RX_BURST_MAX && bHave; i++, bHave = twai_receive(&m, 0) == ESP_OK) {
            if (m.extd) continue;                  /* CANopen: 11-bit only */
            co_frame_t f;
            f.id  = (uint16_t)(m.identifier & 0x7FFu);
            f.dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
            f.rtr = m.rtr != 0;
            memcpy(f.data, m.data, 8);
            co_rx(&s.node, &f, ulNow);
        }

        /* Pump fault -> EMCY (error code 0xFF00 | fault code, manufacturer
         * bytes: fault code, pump state). */
        uint8_t ucPending = s.ucFaultPending;
        if (ucPending) {
            s.ucFaultPending = 0;
            uint8_t mfr[5] = { s.ucFaultCode, DP_REF(Fon_Current_State), 0, 0, 0 };
            uint16_t usCode = ucPending == 1 ? (uint16_t)(CO_EMCY_DEVICE_SPECIFIC | s.ucFaultCode)
                                             : CO_EMCY_NO_ERROR;
            co_emcy(&s.node, usCode, CO_ERR_GENERIC | CO_ERR_DEVICE_SPECIFIC, mfr);
            LOG_EMIT2(LOG_LEVEL_INFO, LOG_MOD_CANOPEN, LOG_EVT_CAN_EMCY,
                      (int32_t)usCode, (int32_t)s.node.error_register, "emcy");
            co_tpdo_request(&s.node, 0);
        }

        co_tick(&s.node, ulNow);
    } while (ulNow - ulStart < CANOPEN_TASK_PERIOD_MS);
    return ESP_OK;
}

bool canopen_recover(void)
{
    if (!s.bHw) return true;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "recover: driver restart");
    stop_all(2);
    s.bAbsent = false;
    if (!DP_REF(Can_Enabled)) return true;
    config_snapshot();
    return start_all();
}
