# clientside_protocol — ESP32-S3 client framework (Fountain v2.2)

Reusable **ESP-IDF component**: Wi-Fi WebSocket client for the Fountain v2.2
protocol with TLS (CA pinning), bearer token, HMAC message authentication
(scope=`control`), datapoint reports and command dispatch — running as a
FreeRTOS task.

Self-contained: the binding to the application/`data_store`/`command` is done
through **callbacks**, not through hard dependencies.

## Modules

| File | Responsibility | Host-testable |
|-------|---------|:------------:|
| `fountain_msgs.{h,c}` *(generated)* | message layer (build/parse, meta) | ✓ |
| `fp_auth.{h,c}` | canonical body, HMAC-SHA256 (mbedTLS), sign/verify, anti-replay | ✓ |
| `fp_envelope.{h,c}` | body ↔ complete wire message | ✓ |
| `fp_session.{h,c}` | handshake/negotiation, auth, dispatch, responses | ✓ |
| `fp_ws.{h,c}` | `esp_websocket_client` wrapper (wss, CA, bearer) | — (ESP-IDF) |
| `fp_task.{h,c}` | FreeRTOS task, timers, `fountain_proto_start()` | — (ESP-IDF) |

The message layer is generated from `fountain_proto_schema/schema.py`
(`python3 generate.py`) — do not edit by hand.

## Integration (ESP-IDF)

Place the component in the project (e.g. as `components/clientside_protocol`).
The component manager pulls `esp_websocket_client` (see `idf_component.yml`).
`json` (cJSON) and `mbedtls` are part of ESP-IDF.

## Usage

```c
#include "fountain_proto.h"

static void fill_snapshot(cJSON *dp, const cJSON *names, void *u) {
    cJSON_AddNumberToObject(dp, "Fon_Current_Pressure", aktueller_druck());
    cJSON_AddNumberToObject(dp, "Fon_Current_State", aktueller_state());
    /* ... or generically from data_store: dp_report_full(dp) ... */
}
static void on_command(const cJSON *m, cJSON *res, void *u) {
    /* map the command onto the data_store/command module, then: */
    cJSON_AddStringToObject(res, "status", "applied");
}
static void on_dp_write(const cJSON *dp, cJSON *res, void *u) {
    /* e.g. call dp_write_batch(dp, errors) */
    cJSON_AddStringToObject(res, "status", "applied");
}

void app_start_proto(void) {
    static const uint8_t key[32] = { /* from NVS/eFuse */ };
    static fp_config_t cfg = {
        .server_host = "server.example.com", .server_port = 443, .server_path = "/ws",
        .ca_pem = SERVER_CA_PEM,
        .device_id = "esp32-a1b2c3d4e5f6", .serial = "000001C0C01FA82A",
        .bearer_token = "…", .auth_kid = "1",
        .auth_key = key, .auth_key_len = 32,
        .fw_version = "2.0.0", .hw_rev = "rev-c",
        .heartbeat_s = 30, .report_s = 10,
        .fill_snapshot = fill_snapshot, .on_command = on_command,
        .on_dp_write = on_dp_write,
    };
    fountain_proto_start(&cfg);   /* Wi-Fi must be brought up separately */
}
```

Proactive alert: `fountain_proto_alert_send("dry_run", "fault", "Fon_Current_Pressure", 0.05, 0.1, "…")`.

## Binding to the existing framework

- **Datapoints:** fill `fill_snapshot` with `dp_report_full()`/`dp_read_into()` from
  `datapoint_organisation/datapoints.h`; `on_dp_write` calls `dp_write_batch()`.
- **Commands:** `on_command` maps onto `command_protocol_map()` + `command_execute()`.
- **Wi-Fi:** bring it up through the existing `wlan_com`; then call `fountain_proto_start()`.

## Auth & tests

HMAC computation: `fountain_proto_schema/AUTH-CONTRACT.md` (golden vector). The
host-testable modules are verified — see `test/host/README.md`
(`GOLDEN VECTOR : OK`, `SESSION TEST OK`).
