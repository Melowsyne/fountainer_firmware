/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* fountain_proto — reusable ESP-IDF component: v2.2 protocol client.
 *
 * Encapsulates WiFi WebSocket communication, handshake/negotiation, HMAC auth
 * (scope=control), datapoint/command dispatch and periodic reports.
 * Self-contained: binding to the application/data_store happens via callbacks,
 * NOT via hard dependencies — this keeps the component reusable.
 *
 * Minimal integration:
 *   fp_config_t cfg = { ... };
 *   fountain_proto_start(&cfg);   // starts the WiFi WS task
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Integration callbacks (provided by the application) ------------ */

/* Fill the datapoint snapshot: populate dp_out with name->value. `names` is the
 * requested list (cJSON array) or NULL/empty for a full snapshot. */
typedef void (*fp_fill_snapshot_t)(cJSON *dp_out, const cJSON *names, void *user);

/* Process an incoming (verified) command. `result_out` is a cJSON
 * object into which the application writes at least "status" ("applied"/"rejected"/"failed"/
 * "deferred") and optionally "error"/"error_detail"/"readback". */
typedef void (*fp_on_command_t)(const cJSON *cmd_msg, cJSON *result_out, void *user);

/* Apply an incoming (verified) dp_write. `result_out`: "status"
 * ("applied"/"rejected") + optional "errors"/"readback". */
typedef void (*fp_on_dp_write_t)(const cJSON *dp, cJSON *result_out, void *user);

/* Connection established + negotiation completed (optional). */
typedef void (*fp_on_ready_t)(void *user);

/* Received a verified ota_available (scope=control, server-attested).
 * The application starts the OTA download (e.g. via esp_https_ota). MUST NOT
 * block — the component calls this from the session context; long
 * work belongs in its own task. `ota_msg` is only valid during the call
 * (copy out url/target_version/sha256/size/crc32). */
typedef void (*fp_on_ota_t)(const cJSON *ota_msg, void *user);

/* --- Configuration -------------------------------------------------------- */
typedef struct {
    /* Connection */
    const char *server_host;      /* e.g. "server.example.com" */
    int         server_port;      /* e.g. 443 / 8443 */
    const char *server_path;      /* e.g. "/ws" (device_id is appended) */
    const char *ca_pem;           /* Server CA (Trust Anchor Pinning), PEM.
                                     NULL -> plaintext ws:// (test only)     */
    const char *client_cert_pem;  /* device certificate for mutual TLS, PEM */
    const char *client_key_pem;   /* device private key for mutual TLS, PEM */

    /* Identity & auth */
    const char *device_id;        /* "esp32-xxxxxxxxxxxx" */
    const char *serial;           /* 16 hex uppercase characters */
    const char *bearer_token;     /* Upgrade header */
    const char *auth_kid;         /* e.g. "1" */
    const uint8_t *auth_key;      /* 256-bit auth key */
    size_t      auth_key_len;     /* 32 */

    const char *fw_version;       /* SemVer */
    const char *hw_rev;           /* optional */

    /* Time intervals */
    uint32_t heartbeat_s;         /* e.g. 30 */
    uint32_t report_s;            /* e.g. 10 (Fon_Report_Interval default) */

    /* Callbacks + user data */
    fp_fill_snapshot_t fill_snapshot;
    fp_on_command_t    on_command;
    fp_on_dp_write_t   on_dp_write;
    fp_on_ready_t      on_ready;
    fp_on_ota_t        on_ota;        /* verified ota_available -> start OTA */

    /* Logging pull (Logging_v1.md): fills the log_batch reply body — records
     * with seq > since_seq (or the previous-boot log if prev_boot), plus
     * boot_id/first_seq_available/next_seq/dropped_count/overflow. */
    void (*fill_log_batch)(cJSON *body_out, uint32_t since_seq,
                           uint8_t min_level, uint16_t max_records,
                           bool prev_boot, void *user);
    /* Server acknowledged the previous-boot log -> slot may be reclaimed. */
    bool (*on_log_ack_prev)(uint32_t boot_id, void *user);

    /* Pressure history (drucksensor_datenstruktur.md): fills the history_batch
     * body — samples with seq > since_seq as [seq,ts_ms,mbar,status] arrays
     * plus next_seq/first_seq_available/overwritten/high_watermark/now_ms. */
    void (*fill_history_batch)(cJSON *body_out, uint32_t since_seq,
                               uint32_t max_samples, void *user);

    /* Progress probe for the app watchdog's WD_SESSION channel: called once
     * per second while the session is negotiated & running (heartbeat source
     * — deadline/escalation live in the app_watchdog, work package 4). */
    void (*on_alive)(void *user);

    /* Edge callback: a previously running session stopped running
     * (disconnect/reset). Counterpart of on_ready. */
    void (*on_session_lost)(void *user);

    /* On-change reporting (optional): fill dp_out with the VOLATILE points
     * that changed since the last report (discrete always, analog past
     * their deadband) and return the count. Polled once per second between
     * the periodic full reports; a dp_report is sent only when count > 0 —
     * state flips (pump on/off, fault, demand) reach the server within
     * ~1 s instead of waiting for the 10/60-s grid. */
    int (*fill_changes)(cJSON *dp_out, void *user);

    /* Optional: physical-link probe (e.g. WLAN associated + IP). Used by the
     * connectivity watchdog to decide whether escalating to a device reboot
     * can help at all (no link -> no reboot; the WLAN layer keeps retrying). */
    bool (*link_up)(void *user);

    void              *user;
} fp_config_t;

/* Starts the protocol component (creates the communication task).
 * The configuration is referenced — it must remain valid. */
bool fountain_proto_start(const fp_config_t *cfg);

/* Proactively sends a device_alert (unsolicited, e.g. on a new fault). */
bool fountain_proto_alert_send(const char *code, const char *severity,
                               const char *datapoint, double value,
                               double threshold, const char *detail);

/* Sends ota_status (c2s, auth=none) — OTA progress/result to the server.
 * Thread-safe (takes the internal mutex); callable from the OTA task. */
bool fountain_proto_ota_status_send(const char *target_version, const char *state,
                                    uint8_t attempt, uint8_t progress_pct,
                                    const char *error, const char *error_detail);

/* Slow mode (power saving): heartbeat + dp_report on a synchronized 60 s
 * grid (one modem wake-up per minute) instead of the configured intervals.
 * Thread-safe toggle; takes effect on the next periodic tick. */
void fountain_proto_slow_mode_set(bool slow);

/* True while the session is negotiated and running (hello_ack accepted). */
bool fountain_proto_running(void);

/* Stage-1 recovery for the app watchdog: hard WS-client restart. */
bool fountain_proto_recover(void);

/* OTA attempt ended WITHOUT reboot (rejected/failed/deferred): switch the
 * session to normal operation (reports/heartbeats), as after ota_none. */
void fountain_proto_ota_failed_note(void);

/* Transmission counters since boot (link-quality metric): frames that left
 * the device vs. frames dropped on a dead/wedged link. */
void fountain_proto_tx_stats(uint32_t *ok, uint32_t *fail);

/* Unix ms (provided by the task via esp_timer/SNTP; 0 allowed before time sync). */
int64_t fp_now_ms(void);

#ifdef __cplusplus
}
#endif
