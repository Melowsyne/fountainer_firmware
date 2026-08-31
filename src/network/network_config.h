/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

/* =============================================================
 * network_config — lifecycle of the Network_* datapoints.
 *
 *  - Defaults come from the embedded network.json (real file is
 *    git-ignored; network.json.example is the committed template)
 *    and are applied when the store is unprovisioned (first boot
 *    or NVS schema change).
 *  - Backup_* points are written ONLY by the system, after a boot
 *    where WLAN + server handshake succeeded (known-good config).
 *  - Network_Save command point: 1 = persist, 2 = persist+reboot,
 *    3 = restore the Backup_* points into Network_*,
 *    4 = TRIAL reboot: apply the (just written) Network_* values,
 *        reboot, and only KEEP them if a full server session comes
 *        up within NETWORK_TRIAL_TIMEOUT_S — otherwise restore the
 *        Backup_* values automatically and reboot again (commit/
 *        confirm like router firmware). The trial counter lives in
 *        its own NVS key, so a power loss inside the window still
 *        ends in a rollback on the next boot.
 * ============================================================= */

/* Network_Save actions (values written by the server via dp_write). */
#define NETWORK_SAVE_PERSIST         1u
#define NETWORK_SAVE_PERSIST_REBOOT  2u
#define NETWORK_SAVE_RESTORE_BACKUP  3u
#define NETWORK_SAVE_TRIAL_REBOOT    4u

/* Confirmation window: boot -> negotiated server session (a healthy boot
 * reaches the session in ~10-15 s; 120 s covers slow DHCP/AP rejoin). */
#define NETWORK_TRIAL_TIMEOUT_S      120u

/* Apply the embedded network.json defaults if the Network_* points are
 * unprovisioned (empty SSID). Call once after dp_init(). */
void network_config_defaults_apply(void);

/* Copy Network_* -> Backup_* and persist. Call ONLY after boot + WLAN +
 * server handshake succeeded (task_com on_ready) — known-good config. */
void network_config_backup(void);

/* Execute a Network_Save action (see defines above). Returns false for an
 * unknown action value (or a trial request without a known-good backup). */
bool network_config_save_handle(uint8_t ucAction);

/* Trial lifecycle (Network_Save=4). boot_check: call once after
 * defaults_apply and BEFORE the WLAN connect — arms the window on a trial
 * boot, or restores the backup immediately if a previous trial never got
 * confirmed (power loss inside the window). tick: from the monitor (5 s);
 * fires the rollback+reboot on timeout. confirm: on EVT_SESSION_READY. */
void network_config_trial_boot_check(void);
void network_config_trial_tick(void);
void network_config_trial_confirm(void);
