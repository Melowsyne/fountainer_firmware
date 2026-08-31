/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "network_config.h"
#include "system.h"
#include "datapoints.h"
#include "event_manager.h"
#include "debug.h"

#include <string.h>
#include "cJSON.h"
#include "nvs.h"
#include "esp_timer.h"

#define TAG "net_cfg"

/* Trial state machine, persisted in an OWN NVS namespace so it survives
 * datapoint schema bumps AND power losses inside the trial window:
 *   (absent)/0  no trial
 *   1           trial requested (set by Network_Save=4, then reboot)
 *   2           trial window armed on a boot — if we EVER boot again and
 *               still see 2, the window was never confirmed (power loss /
 *               crash with the new credentials) -> immediate rollback
 *   3           rollback performed — marker for the FOLLOWING boot to
 *               surface Network_Trial_State=3, then erased               */
#define TRIAL_NVS_NAMESPACE "netcfg"
#define TRIAL_NVS_KEY       "trial"

static bool     s_bTrialActive;        /* window running in THIS boot      */
static uint32_t s_ulTrialDeadlineS;    /* uptime deadline for confirmation */

static uint32_t up_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }

static uint8_t trial_nvs_get(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(TRIAL_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_u8(h, TRIAL_NVS_KEY, &v);
    nvs_close(h);
    return v;
}

static void trial_nvs_set(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(TRIAL_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (v) nvs_set_u8(h, TRIAL_NVS_KEY, v);
    else   nvs_erase_key(h, TRIAL_NVS_KEY);
    nvs_commit(h);
    nvs_close(h);
}

/* network.json content, generated into src/network_json_gen.c at build time
 * by tools/ensure_network_json.py (real file is git-ignored; a fresh checkout
 * gets a copy of the committed network.json.example). */
extern const char g_network_json[];

/* ----- small helpers --------------------------------------------------- */

/* Copy a bounded string into a DP_STR slot (always NUL-terminated). */
#define DP_STR_SET(nm, src)                                     \
    do {                                                        \
        strncpy(DP_REF(nm), (src), sizeof(DP_REF(nm)) - 1);     \
        DP_REF(nm)[sizeof(DP_REF(nm)) - 1] = '\0';              \
    } while (0)

static const char *json_str(const cJSON *root, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

/* ----- defaults from the embedded network.json ------------------------- */

void network_config_defaults_apply(void)
{
    /* Provisioning marker: an empty SSID means "never provisioned" (first
     * boot, flash erase, or NVS schema change rejected the stored blob). */
    if (DP_REF(Network_SSID)[0] != '\0') return;

    cJSON *pRoot = cJSON_Parse(g_network_json);
    if (!pRoot) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "embedded network.json unparsable — keeping compiled defaults");
        return;
    }

    const char *s;
    if ((s = json_str(pRoot, "Network_SSID")))       DP_STR_SET(Network_SSID, s);
    if ((s = json_str(pRoot, "Network_Password")))   DP_STR_SET(Network_Password, s);
    if ((s = json_str(pRoot, "Network_Server")))     DP_STR_SET(Network_Server, s);
    if ((s = json_str(pRoot, "Network_IP_Address"))) DP_STR_SET(Network_IP_Address, s);
    if ((s = json_str(pRoot, "Network_Subnetmask"))) DP_STR_SET(Network_Subnetmask, s);
    if ((s = json_str(pRoot, "Network_Gateway")))    DP_STR_SET(Network_Gateway, s);

    const cJSON *it = cJSON_GetObjectItemCaseSensitive(pRoot, "Network_DHCP");
    if (cJSON_IsBool(it)) DP_REF(Network_DHCP) = cJSON_IsTrue(it) ? 1 : 0;
    it = cJSON_GetObjectItemCaseSensitive(pRoot, "Network_Server_Port");
    if (cJSON_IsNumber(it)) DP_REF(Network_Server_Port) = (uint16_t)it->valuedouble;

    cJSON_Delete(pRoot);

    if (dp_config_save() == ESP_OK)
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "defaults applied from network.json (ssid=%s, server=%s, dhcp=%d)",
                DP_REF(Network_SSID), DP_REF(Network_Server), (int)DP_REF(Network_DHCP));
    else
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "defaults applied from network.json (NVS save failed — RAM only)");
}

/* ----- known-good backup ------------------------------------------------ */

void network_config_backup(void)
{
    /* Skip the NVS write if the backup already matches (flash wear). */
    if (DP_REF(Backup_DHCP) == DP_REF(Network_DHCP) &&
        DP_REF(Backup_Server_Port) == DP_REF(Network_Server_Port) &&
        strcmp(DP_REF(Backup_IP_Address), DP_REF(Network_IP_Address)) == 0 &&
        strcmp(DP_REF(Backup_Subnetmask), DP_REF(Network_Subnetmask)) == 0 &&
        strcmp(DP_REF(Backup_Gateway),    DP_REF(Network_Gateway))    == 0 &&
        strcmp(DP_REF(Backup_Server),     DP_REF(Network_Server))     == 0 &&
        strcmp(DP_REF(Backup_SSID),       DP_REF(Network_SSID))       == 0 &&
        strcmp(DP_REF(Backup_Password),   DP_REF(Network_Password))   == 0) {
        logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG, "backup unchanged");
        return;
    }

    dp_lock(portMAX_DELAY);
    DP_REF(Backup_DHCP)        = DP_REF(Network_DHCP);
    DP_REF(Backup_Server_Port) = DP_REF(Network_Server_Port);
    DP_STR_SET(Backup_IP_Address, DP_REF(Network_IP_Address));
    DP_STR_SET(Backup_Subnetmask, DP_REF(Network_Subnetmask));
    DP_STR_SET(Backup_Gateway,    DP_REF(Network_Gateway));
    DP_STR_SET(Backup_Server,     DP_REF(Network_Server));
    DP_STR_SET(Backup_SSID,       DP_REF(Network_SSID));
    DP_STR_SET(Backup_Password,   DP_REF(Network_Password));
    dp_unlock();

    if (dp_config_save() == ESP_OK)
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "network config backed up (known-good after WLAN + server ok)");
    else
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "backup NVS save failed");
}

/* ----- Network_Save actions --------------------------------------------- */

static void restore_from_backup(void)
{
    dp_lock(portMAX_DELAY);
    DP_REF(Network_DHCP)        = DP_REF(Backup_DHCP);
    DP_REF(Network_Server_Port) = DP_REF(Backup_Server_Port);
    DP_STR_SET(Network_IP_Address, DP_REF(Backup_IP_Address));
    DP_STR_SET(Network_Subnetmask, DP_REF(Backup_Subnetmask));
    DP_STR_SET(Network_Gateway,    DP_REF(Backup_Gateway));
    DP_STR_SET(Network_Server,     DP_REF(Backup_Server));
    DP_STR_SET(Network_SSID,       DP_REF(Backup_SSID));
    DP_STR_SET(Network_Password,   DP_REF(Backup_Password));
    dp_unlock();
}

bool network_config_save_handle(uint8_t ucAction)
{
    switch (ucAction) {
    case NETWORK_SAVE_PERSIST:
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "Network_Save=1: persist");
        dp_config_save();
        event_manager_publish(EVT_NETWORK_CONFIG_SAVED, &ucAction, sizeof(ucAction));
        return true;

    case NETWORK_SAVE_PERSIST_REBOOT:
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "Network_Save=2: persist + reboot");
        dp_config_save();
        event_manager_publish(EVT_NETWORK_CONFIG_SAVED, &ucAction, sizeof(ucAction));
        /* Deferred via core/system (dp_write_result goes out first, then the
         * main-injected pre-reboot hook + esp_restart). */
        return system_reboot_deferred(800);

    case NETWORK_SAVE_RESTORE_BACKUP:
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG, "Network_Save=3: restore backup");
        restore_from_backup();
        dp_config_save();
        event_manager_publish(EVT_NETWORK_CONFIG_RESTORED, NULL, 0);
        return true;

    case NETWORK_SAVE_TRIAL_REBOOT:
        /* Commit/confirm: without a known-good backup a failed trial could
         * not roll back to anything -> refuse (dp_write_result: rejected). */
        if (DP_REF(Backup_SSID)[0] == '\0') {
            logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                    "Network_Save=4: REFUSED — no known-good backup yet");
            return false;
        }
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "Network_Save=4: trial reboot (ssid='%s'; rollback to '%s' "
                "unless a session comes up within %u s)",
                DP_REF(Network_SSID), DP_REF(Backup_SSID),
                (unsigned)NETWORK_TRIAL_TIMEOUT_S);
        dp_config_save();                 /* new values ride into the boot  */
        trial_nvs_set(1);                 /* arm: next boot runs the trial  */
        event_manager_publish(EVT_NETWORK_CONFIG_SAVED, &ucAction, sizeof(ucAction));
        return system_reboot_deferred(800);

    default:
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "Network_Save=%u: unknown action", (unsigned)ucAction);
        return false;
    }
}

/* ----- trial lifecycle (Network_Save=4) ---------------------------------- */

static void trial_rollback(const char *pstrWhy)
{
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "TRIAL FAILED (%s) — restoring known-good config (ssid='%s')",
            pstrWhy, DP_REF(Backup_SSID));
    restore_from_backup();
    dp_config_save();
    trial_nvs_set(3);                     /* marker: surface state=3 next boot */
    event_manager_publish(EVT_NETWORK_CONFIG_RESTORED, NULL, 0);
}

void network_config_trial_boot_check(void)
{
    switch (trial_nvs_get()) {
    case 1:                               /* this IS the trial boot */
        trial_nvs_set(2);                 /* power-loss guard: an unconfirmed
                                             2 on a later boot = rollback   */
        s_bTrialActive = true;
        s_ulTrialDeadlineS = up_s() + NETWORK_TRIAL_TIMEOUT_S;
        DP_REF(Network_Trial_State) = 1;
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "TRIAL boot: testing ssid='%s' (%u s to confirm via server "
                "session, else rollback to '%s')", DP_REF(Network_SSID),
                (unsigned)NETWORK_TRIAL_TIMEOUT_S, DP_REF(Backup_SSID));
        break;
    case 2:                               /* trial never confirmed: power was
                                             lost inside the window */
        trial_rollback("power loss during trial window");
        DP_REF(Network_Trial_State) = 3;
        trial_nvs_set(0);                 /* rollback done + state surfaced */
        break;                            /* boot continues with OLD config */
    case 3:                               /* previous boot rolled back */
        DP_REF(Network_Trial_State) = 3;
        trial_nvs_set(0);
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "running on ROLLED-BACK config (last trial failed)");
        break;
    default:
        break;
    }
}

void network_config_trial_tick(void)
{
    if (!s_bTrialActive || up_s() < s_ulTrialDeadlineS) return;
    s_bTrialActive = false;
    trial_rollback("no server session within the window");
    /* Reboot to apply the restored credentials (WLAN runs on the trial
     * ones right now); the next boot surfaces Network_Trial_State=3. */
    system_reboot_deferred(1000);
}

void network_config_trial_confirm(void)
{
    if (!s_bTrialActive) return;
    s_bTrialActive = false;
    trial_nvs_set(0);
    DP_REF(Network_Trial_State) = 2;
    logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
            "TRIAL CONFIRMED: server session up — new WLAN config is now "
            "permanent (backup follows on this session)");
}
