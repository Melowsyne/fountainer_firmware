/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "wlan_com.h"
#include "net_utils.h"
#include "datapoints.h"
#include "event_manager.h"
#include "debug.h"

#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#define TAG "wlan"
/* GENTLE, escalating reconnect backoff (do not block in the event task):
 * FEW attempts per time, so that the AP does not trigger an anti-DoS/association
 * lockout. Fast (sub-second) retries flood the AP -> it then refuses EVERY
 * association of this device with status-30; a lockout arises that sometimes only
 * an AP restart clears. Therefore start generously, escalate strongly and finish
 * with a long quiet window (up to 2 min) that gives a lockout time to
 * expire: 5,10,20,40,80,120 s … reset to MIN on obtaining an IP. */
#define WLAN_RECONNECT_BACKOFF_MIN_MS  5000
#define WLAN_RECONNECT_BACKOFF_MAX_MS  120000

/* =============================================================
 * wlan_com — WLAN connection setup, event-driven.
 * WLAN driver and lwIP run in ESP-IDF-internal tasks; this
 * module only registers event handlers and initiates connect
 * (no own polling task, non-blocking).
 * ============================================================= */

static wlan_state_cb_t s_pfnCb;
static void           *s_pvCbCtx;
static wlan_state_t    s_eState = WLAN_DISCONNECTED;
static int               s_slRetry = 0;
static bool              s_bInited = false;
static esp_timer_handle_t s_hReconnTimer;
static uint32_t          s_ulBackoffMs = WLAN_RECONNECT_BACKOFF_MIN_MS;
static EventGroupHandle_t s_hEvents;            /* signals STA_DISCONNECTED */
static volatile bool     s_bShutdown = false;   /* clean teardown before reboot in progress */
static uint32_t          s_ulDisconnects = 0;   /* unexpected link losses since boot */
static esp_netif_t      *s_pNetif;              /* STA netif (for static IP config) */
#define WLAN_EVT_DISCONNECTED  (1 << 0)

static void set_state(wlan_state_t eState)
{
    s_eState = eState;
    logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG, "state=%d", (int)eState);
    if (s_pfnCb) s_pfnCb(eState, s_pvCbCtx);
}

/* Timer callback: initiates the next connection attempt (outside sys_evt). */
static void reconn_timer_cb(void *pvArg)
{
    (void)pvArg;
    esp_wifi_connect();
}

static void event_handler(void *pvArg, esp_event_base_t eBase,
                          int32_t slId, void *pvData)
{
    (void)pvArg;
    if (eBase == WIFI_EVENT && slId == WIFI_EVENT_STA_START) {
        set_state(WLAN_CONNECTING);
        esp_wifi_connect();

    } else if (eBase == WIFI_EVENT && slId == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *pDis = (wifi_event_sta_disconnected_t *)pvData;
        if (pDis)
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "Disconnect reason=%d (15=WrongPassword/4WAY, 201=NO_AP, 2=AUTH_EXPIRE)",
                    (int)pDis->reason);
        if (s_hEvents) xEventGroupSetBits(s_hEvents, WLAN_EVT_DISCONNECTED);
        if (s_bShutdown) return;   /* clean teardown before reboot -> no reconnect */
        s_ulDisconnects++;         /* unexpected loss (stability metric) */
        {
            uint8_t ucReason = pDis ? (uint8_t)pDis->reason : 0;
            event_manager_publish(EVT_WLAN_DISCONNECTED, &ucReason, sizeof(ucReason));
        }
        set_state(WLAN_CONNECTING);
        s_slRetry++;
        /* Escalating backoff instead of immediate retry (prevents AP "Association
         * refused"); do NOT block in the event task -> via one-shot timer. */
        logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG,
                "reconnect in %u ms (attempt %d)", (unsigned)s_ulBackoffMs, s_slRetry);
        esp_timer_stop(s_hReconnTimer);
        esp_timer_start_once(s_hReconnTimer, (uint64_t)s_ulBackoffMs * 1000);
        /* double for the next failed attempt (up to MAX). */
        s_ulBackoffMs *= 2;
        if (s_ulBackoffMs > WLAN_RECONNECT_BACKOFF_MAX_MS)
            s_ulBackoffMs = WLAN_RECONNECT_BACKOFF_MAX_MS;

    } else if (eBase == IP_EVENT && slId == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *pEv = (ip_event_got_ip_t *)pvData;
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "connected, IP " IPSTR,
                IP2STR(&pEv->ip_info.ip));
        /* Mirror the EFFECTIVE addressing (DHCP lease or confirmed static)
         * into the Network_* datapoints, so the server always sees it. */
        dp_lock(portMAX_DELAY);
        snprintf(DP_REF(Network_IP_Address), sizeof(DP_REF(Network_IP_Address)),
                 IPSTR, IP2STR(&pEv->ip_info.ip));
        snprintf(DP_REF(Network_Subnetmask), sizeof(DP_REF(Network_Subnetmask)),
                 IPSTR, IP2STR(&pEv->ip_info.netmask));
        snprintf(DP_REF(Network_Gateway), sizeof(DP_REF(Network_Gateway)),
                 IPSTR, IP2STR(&pEv->ip_info.gw));
        dp_unlock();
        s_slRetry = 0;
        s_ulBackoffMs = WLAN_RECONNECT_BACKOFF_MIN_MS;   /* reset backoff */
        {
            uint32_t ulIp = pEv->ip_info.ip.addr;
            event_manager_publish(EVT_WLAN_CONNECTED, &ulIp, sizeof(ulIp));
        }
        set_state(WLAN_CONNECTED);
    }
}

/* Apply the addressing mode from the Network_* datapoints: DHCP client (the
 * default), or a static configuration from Network_IP_Address/Subnetmask/
 * Gateway. The strings may be IP literals or (for future use) DNS names —
 * pre-connect only literals can resolve, so an unresolvable static config
 * falls back to DHCP instead of leaving the device unreachable. */
static void addressing_apply(void)
{
    if (!s_pNetif) return;

    if (DP_REF(Network_DHCP)) {
        esp_err_t e = esp_netif_dhcpc_start(s_pNetif);   /* idempotent-ish */
        if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "dhcpc_start err=%d", (int)e);
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "addressing: DHCP");
        return;
    }

    esp_netif_ip_info_t stIp = {0};
    if (!net_util_resolve_ip4(DP_REF(Network_IP_Address), &stIp.ip) ||
        !net_util_resolve_ip4(DP_REF(Network_Subnetmask), &stIp.netmask) ||
        !net_util_resolve_ip4(DP_REF(Network_Gateway),    &stIp.gw)) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "static config unresolvable (ip='%s' mask='%s' gw='%s') -> DHCP fallback",
                DP_REF(Network_IP_Address), DP_REF(Network_Subnetmask),
                DP_REF(Network_Gateway));
        esp_netif_dhcpc_start(s_pNetif);
        return;
    }

    esp_netif_dhcpc_stop(s_pNetif);      /* err if already stopped is fine */
    if (esp_netif_set_ip_info(s_pNetif, &stIp) == ESP_OK)
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "addressing: static " IPSTR
                " gw " IPSTR, IP2STR(&stIp.ip), IP2STR(&stIp.gw));

    /* Static addressing brings NO DNS server (DHCP would). Use the gateway
     * as resolver so a Network_Server DOMAIN still resolves (net_utils). */
    esp_netif_dns_info_t stDns = { .ip = { .type = ESP_IPADDR_TYPE_V4 } };
    stDns.ip.u_addr.ip4 = stIp.gw;
    esp_netif_set_dns_info(s_pNetif, ESP_NETIF_DNS_MAIN, &stDns);
}

bool wlan_com_init(void)
{
    if (s_bInited) return true;

    s_hEvents = xEventGroupCreate();
    if (!s_hEvents) return false;

    const esp_timer_create_args_t stTimer = {
        .callback = reconn_timer_cb, .name = "wlan_reconn",
    };
    if (esp_timer_create(&stTimer, &s_hReconnTimer) != ESP_OK) return false;

    if (esp_netif_init() != ESP_OK)                  return false;
    if (esp_event_loop_create_default() != ESP_OK)   return false;
    s_pNetif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK)               return false;

    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
            &event_handler, NULL, NULL) != ESP_OK)   return false;
    if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            &event_handler, NULL, NULL) != ESP_OK)   return false;

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)  return false;

    /* Set country/channel range explicitly to DE (channels 1–13). The default
     * after a flash erase is "01" (only 1–11). If the router picks channel 12/13
     * via auto-channel, an ESP limited to 1–11 cannot associate there
     * -> association fails, while already connected devices
     * (which already have the channel) keep running. MANUAL enforces 1–13. */
    wifi_country_t stCountry = {
        .cc = "DE", .schan = 1, .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    esp_wifi_set_country(&stCountry);

    s_bInited = true;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "init");
    return true;
}

bool wlan_com_connect(const char *pstrSsid, const char *pstrPassword)
{
    if (!pstrSsid || !pstrPassword) return false;
    if (pstrSsid[0] == '\0') {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "no SSID configured — WLAN inactive (set datapoint Network_SSID)");
        return false;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            /* Associate as a pure WPA2-PSK client: do NOT require/advertise PMF.
             * Together with CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n the
             * RSNXE/SAE/PMF elements are omitted from the assoc request, on which a
             * WPA2/WPA3-mixed router chokes (permanent status-30 refusal). */
            .pmf_cfg = { .capable = false, .required = false },
            /* All-channel scan with directed probes: required to find a HIDDEN
             * SSID reliably (the AP does not beacon its name), and robust
             * against autochannel moves. */
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
        },
    };
    strncpy((char *)wifi_config.sta.ssid,     pstrSsid,     sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pstrPassword, sizeof(wifi_config.sta.password) - 1);

    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) return false;

    /* Addressing per the Network_* datapoints (DHCP or static), applied
     * BEFORE esp_wifi_start(); IP_EVENT_STA_GOT_IP fires in both modes. */
    addressing_apply();

    s_slRetry     = 0;
    s_bShutdown   = false;                            /* fresh start: no teardown active */
    s_ulBackoffMs = WLAN_RECONNECT_BACKOFF_MIN_MS;    /* backoff fresh */
    if (esp_wifi_start() != ESP_OK) return false;    /* triggers WIFI_EVENT_STA_START */

    /* Power-save OFF: more reliable association + 4-way handshake. The device
     * is mains-powered; WIFI_PS_MIN_MODEM (default) lets the STA sleep between
     * beacons and may miss auth/assoc responses -> repeated
     * failed attempts/unstable connection. Constantly awake = more robust. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "connect ssid=%s (PS=none)", pstrSsid);
    return true;                                      /* non-blocking; events take over */
}

void wlan_com_disconnect_blocking(uint32_t ulTimeoutMs)
{
    /* Clean WLAN teardown before a software reboot: send a (under PMF protected)
     * deauth to the AP and wait for the confirmation (STA_DISCONNECTED),
     * so that the frame ACTUALLY goes out (esp_restart() would otherwise choke it).
     * This makes the AP release the 802.11w SA immediately -> after the reboot no
     * "Association refused / comeback time" (otherwise ~3 min). Source: esp-idf #9428. */
    if (!s_bInited) return;
    s_bShutdown = true;                                  /* no more auto-reconnect */
    if (s_hEvents) xEventGroupClearBits(s_hEvents, WLAN_EVT_DISCONNECTED);
    esp_timer_stop(s_hReconnTimer);
    esp_wifi_disconnect();
    if (s_hEvents)
        xEventGroupWaitBits(s_hEvents, WLAN_EVT_DISCONNECTED, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(ulTimeoutMs));
    vTaskDelay(pdMS_TO_TICKS(150));   /* settle time: frame safely sent */
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "WLAN cleanly disconnected (deauth) before reboot");
}

void wlan_com_teardown_for_reboot(uint32_t ulTimeoutMs)
{
    wlan_com_disconnect_blocking(ulTimeoutMs);
    esp_wifi_stop();
}

bool wlan_com_connected_get(void) { return s_eState == WLAN_CONNECTED; }

uint32_t wlan_com_disconnect_count_get(void) { return s_ulDisconnects; }

void wlan_com_ps_low_set(bool bLow)
{
    if (!s_bInited) return;
    esp_wifi_set_ps(bLow ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG, "power save -> %s",
            bLow ? "MIN_MODEM" : "NONE");
}

bool wlan_com_saved_credentials_get(char *pstrSsid, size_t szSsid,
                                    char *pstrPassword, size_t szPassword)
{
    if (!pstrSsid || szSsid == 0 || !pstrPassword || szPassword == 0) return false;
    if (!s_bInited) return false;

    wifi_config_t stCfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &stCfg) != ESP_OK) return false;
    if (stCfg.sta.ssid[0] == '\0') return false;

    strncpy(pstrSsid, (const char *)stCfg.sta.ssid, szSsid - 1);
    pstrSsid[szSsid - 1] = '\0';
    strncpy(pstrPassword, (const char *)stCfg.sta.password, szPassword - 1);
    pstrPassword[szPassword - 1] = '\0';

    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "using stored ESP WLAN credentials as fallback: ssid=%s",
            pstrSsid);
    return true;
}

void wlan_com_state_cb_set(wlan_state_cb_t pfnCb, void *pvCtx)
{
    s_pfnCb  = pfnCb;
    s_pvCbCtx = pvCtx;
}
