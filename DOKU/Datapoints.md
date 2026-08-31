# Datapoints — Engine and Catalog Reference

Component: `src/components/datapoints/` (`datapoints.h`, `datapoints.c`,
`dp_list.def`). The datapoint system is the single state interface between
the firmware and the server: everything the server can observe or configure
is a datapoint.

---

## 1. X-macro engine

`dp_list.def` is the **single source of truth**. One line

```c
DP(name, type, access, persist, id, default, min, max, deadband)
```

expands into four artifacts at compile time:

1. the id enum (`DP_ID_<name>`),
2. a member in the packed RAM image struct (`g_dp_store`, one contiguous
   block of ~150 bytes),
3. an entry in the flash-resident descriptor table `g_dp[]` (type, access,
   persistence, limits, deadband),
4. the NVS default applied on first boot.

Adding a datapoint is a one-line change; the linter
(`tools/lint_datapoints.py`, a PlatformIO pre-build step) validates the
catalog and exports `datapoints_meta.json` for the server UI. Retired ids are
tracked so they are never reused.

**Types:** `BOOL U8 U16 U32 U64 I8 I16 I32 F32 ENUM STR`.
Wire encoding (`dp_value_to_json`): numbers → JSON numbers, `BOOL` → JSON
bool, `STR` → string, `ENUM` → number, `U64` → 16-digit uppercase hex string
(`%016llX`, used for the serial number).

**Access:** `RO` (device-owned), `RW` (server-writable), `WO` (command
points). **Persistence:** `VOLATILE` (RAM only), `NVS` (one NVS key per
point, `d<id>` — values survive schema/layout changes), `STATIC` (identity,
set once at init).

**Concurrency:** scalar updates by the single writing task are lock-free;
multi-field operations (snapshot, batch write) take the store mutex
(`dp_lock`/`dp_unlock`). Application code uses the typed `DP_REF(name)`
accessor; the protocol layer uses raw `DP_GET`/`DP_SET` plus `dp_find` by
name.

## 2. Reporting

- **Full snapshot** — every `report_s` (10 s; 60 s in slow mode) the protocol
  sends all `VOLATILE` points and resets the on-change baseline
  (`dp_report_full`).
- **On-change deltas** — checked at 1 Hz between full reports
  (`dp_report_changes`). Eligible: `BOOL`/`ENUM` on any change, `F32` when a
  non-zero `deadband` is configured and `|cur − last| ≥ deadband`. Analog
  points without a deadband are periodic-only by design (noise would flood
  the link).
- **`dp_read`** — named list → targeted read; empty list → full snapshot.

## 3. Writes

`dp_write` batches are **atomic all-or-nothing** (`dp_write_batch`): every
value is checked for type and range, then the application cross-field
validator `dp_constraints_ok` runs, and only if everything passes is the
batch applied and NVS-persisted. The result carries per-name error codes
(`type_mismatch` / `out_of_range`), a `readback{}` of the written values, and
a `warning: "nvs_save_failed"` if a value applied in RAM but could not be
persisted.

Cross-field constraints (pump safety):

- `Fon_Min_Pressure < Fon_Max_Pressure < Fon_Alert_High_Pressure`
- `Fon_Alert_Low_Pressure < Fon_Min_Pressure`
- `Fon_Max_On_Time ≥ 10`, `Fon_Min_On_Time < Fon_Max_On_Time`
- `Fon_Dry_Run_Detect_Time < Fon_Max_On_Time`
- `Fon_Report_Interval` ∈ 1…3600

Two write targets are intercepted before the batch and executed as
**commands** rather than stored values: `Network_Save` and `Log_Command`
(see §4, groups *Network* and *Logging*).

## 4. Catalog — complete reference

All 102 datapoints, grouped as in `dp_list.def`. `[id]` = stable NVS id for
persisted points (their NVS key is `d<id>`; ids are never reused).

### Device identity (RO, STATIC — fixed at boot)

| Datapoint | Type | Purpose |
|---|---|---|
| `Device_Serial_Number` | U64 | Unique device serial; sent as 16-digit uppercase hex string on the wire. |
| `Device_HW_Version` | STR | Hardware revision of the board. |
| `Device_SW_Version` | STR | Firmware version (from `version.txt`, same string the OTA compare uses). |
| `Device_Build_Version` | U64 | Unix timestamp (ms) of the firmware build, embedded by `tools/gen_build_info.py` — distinguishes two builds of the same version. |

### System health (RO, VOLATILE — refreshed by the 5 s monitor cycle)

| Datapoint | Type | Purpose |
|---|---|---|
| `System_Temperature` | F32 (°C, deadband 1.0) | Internal chip temperature of the ESP32-S3. |
| `System_Utilization` | U8 (%) | CPU utilization (FreeRTOS runtime stats). |
| `System_Memory_Free` | U32 (B) | Currently free heap. |
| `System_Min_Memory_Free` | U32 (B) | Low-water mark of free heap since boot (leak indicator). |
| `System_Flash_Free` | U32 (B) | Free flash space. |
| `System_RSSI` | I8 (dBm) | Wi-Fi signal strength. |
| `System_Uptime` | U32 (s) | Time since boot. |
| `System_Reconnect_Count` | U32 | Unexpected Wi-Fi link losses since boot (each schedules a backoff reconnect). |
| `System_Reset_Reason` | ENUM | Last reset cause: 1 power-on, 3 software, 4 panic, 5–7 watchdog, 9 brownout. |
| `System_Power_Mode` | U8 | 0 = HIGH (160 MHz, no power save), 1 = LOW (80 MHz DFS, modem sleep, 60 s protocol grid). |
| `System_Event_Drops` | U32 | Event-manager queue overflows since boot — should stay 0. |

### Watchdog boot diagnosis (RO, VOLATILE — filled after a watchdog reboot)

| Datapoint | Type | Purpose |
|---|---|---|
| `System_WD_Last_Channel` | U8 | Which supervision channel escalated the last watchdog reboot. |
| `System_WD_Last_Checkpoint` | U16 | Last checkpoint that channel reached before stalling. |
| `System_WD_Reboot_Count` | U8 | Reboot-brake counter (reboots without a successful session; at 3 the watchdog stops rebooting until power cycle). |

### Link quality (RO, VOLATILE — Link_Robustness_v1)

| Datapoint | Type | Purpose |
|---|---|---|
| `Net_Link_Score` | U8 | Link quality 0–100 (RSSI base curve minus penalties, EMA-smoothed). |
| `Net_Link_State` | U8 | 0 = GOOD, 1 = POOR (hysteresis: POOR < 40, GOOD > 55). POOR gates OTA and enables slow mode. |
| `Net_Session_Drops` | U32 | Lost protocol sessions since boot. |
| `Net_Send_Fail_Count` | U32 | Failed frame transmissions. |
| `Net_Offline_Seconds` | U32 (s) | Cumulative time without a session. |
| `Net_Last_Offline_S` | U32 (s) | Duration of the most recent offline period. |

### Ambient climate (RO, VOLATILE — AM2302 on GPIO 4, 60 s interval)

| Datapoint | Type | Purpose |
|---|---|---|
| `Ambient_Temperature` | F32 (°C, deadband 0.2) | Air temperature at the installation site. |
| `Ambient_Humidity` | F32 (%rH, deadband 1.0) | Relative humidity at the installation site. |

### Fountain measurement & status (RO, VOLATILE — written by the pump module)

| Datapoint | Type | Purpose |
|---|---|---|
| `Fon_Current_Pressure` | F32 (bar, deadband 0.05) | Current line pressure (calibrated). |
| `Fon_Sensor_Voltage_mV` | U32 (mV) | Raw sensor voltage; < 500 mV (the 0-PSI point) indicates a broken wire or sensor. |
| `Fon_Current_State` | ENUM | Current state of the pump state machine (Off/On/Auto/Manual plus internal states). |
| `Fon_Relay_Output` | BOOL | Actual SSR/pump relay output. |
| `Fon_Run_Time` | U32 (s) | Runtime of the current/last pump run. |
| `Fon_Cycles_Total` | U32 | Total pump start cycles. |
| `Fon_Remaining_Time` | U32 (s) | Remaining runtime of a timed run (`turn_on_duration`). |
| `Fon_Pressure_Filtered` | F32 (bar, deadband 0.05) | EMA-filtered pressure (regulation input). |
| `Fon_Pressure_Slope` | F32 (bar/s, deadband 0.01) | Pressure gradient — rise/fall rate. |
| `Fon_Demand_State` | ENUM | Demand classification: 0 none, 1 unknown, 2 hand valve, 3 tank draw, 4 leak suspect, 5 pipe break, 6 sensor. |
| `Fon_Anomaly_Score` | U16 | Anomaly score of the pump-manager observation layer. |
| `Fon_Event_Duration` | U32 (s) | Duration of the current demand event. |
| `Fon_Est_Flow_L_Min` | F32 (l/min, deadband 0.1) | Estimated flow (requires calibrated k-factor). |
| `Fon_Est_Volume_Total` | F32 (l, deadband 0.5) | Estimated cumulative delivered volume. |
| `Fon_Fault_Code` | U8 | Latched fault: 0 none, 1 sensor, 2 critical-low, 3 max-runtime, 4 dry-run, 5 starts/hour. |
| `Fon_Starts_Per_Hour` | U8 (/h) | Pump starts in the last hour (wear guard input). |
| `Fon_Sensor_Err_Count` | U32 | Cumulative failed pressure readings (I2C glitches etc.); a sensor fault only latches after 3 consecutive misses. |
| `Fon_Sensor_Noise_mV` | U16 (mV) | Spread (max−min) of the 4-sample ADC burst — early indicator of wiring/grounding issues. |

### Fountain controls (RW, VOLATILE — command-style, polled by the pump task)

| Datapoint | Type | Range | Purpose |
|---|---|---|---|
| `Fon_Fault_Ack` | U8 | 0–1 | Write 1 to acknowledge a latched pump fault (requires a healthy sensor); always reads 0. |
| `Fon_Event_Label` | U8 | 0–7 | Operator classification of the current/last demand event (0 unknown, 1 none, 2 hand, 3 tank, 4 leak, 5 break, 6 sensor, 7 maintenance); logged on change. |
| `Fon_Pressure_Manual` | BOOL | — | 1 = simulation override: `Fon_Pressure_Value` feeds the control chain instead of the sensor. Volatile on purpose — every reboot returns to the real sensor, a stale simulation can never drive the pump. |
| `Fon_Pressure_Value` | F32 | 0–100 bar | Simulated pressure used while the override is active. |

### Fountain configuration (RW, NVS, ids 101–127)

| Datapoint | [id] | Default | Range | Purpose |
|---|---|---|---|---|
| `Fon_Min_Pressure` | 101 | 2.0 bar | 0–10 | AUTO mode: pump starts below this pressure. |
| `Fon_Max_Pressure` | 102 | 3.5 bar | 0–10 | AUTO mode: pump stops at this pressure. |
| `Fon_Alert_High_Pressure` | 103 | 4.5 bar | 0–12 | Overpressure alert threshold. |
| `Fon_Alert_Low_Pressure` | 104 | 0.3 bar | 0–10 | Critically-low-pressure alert threshold. |
| `Fon_Min_On_Time` | 105 | 30 s | 0–3600 | Minimum runtime once started (anti-chatter). |
| `Fon_Max_On_Time` | 106 | 300 s | 10–65535 | Safety runtime cutoff → max-runtime fault. |
| `Fon_Dry_Run_Detect_Time` | 107 | 30 s | 1–3600 | Window after start in which pressure must rise. |
| `Fon_Dry_Run_Min_Rise` | 108 | 100 mbar | 0–10000 | Minimum pressure rise within that window; less → dry-run fault. |
| `Fon_Check_Valve_Timeout` | 109 | 10 s | 1–3600 | Check-valve supervision timeout. |
| `Fon_Pressure_Drop_Rate` | 110 | 500 | 0–65535 | Pressure-drop-rate threshold for demand/leak detection. |
| `Fon_Report_Interval` | 111 | 10 s | 1–3600 | Reporting interval of the pump values. |
| `Fon_Sensor_Offset` | 112 | 0 mbar | −5000…5000 | Additive sensor calibration. |
| `Fon_Sensor_Scale` | 113 | 1.0 | — | Multiplicative sensor calibration. |
| `Fon_Sensor_Range_Bar` | 114 | 34.47 bar | 0.1–100 | Sensor full scale (0.5–4.5 V → 0–500 PSI); curve: `bar = (V−0.5)/4.0 × Range × Scale + Offset`. |
| `Fon_Min_Off_Time` | 115 | 30 s | 0–3600 | Minimum pause between two runs. |
| `Fon_Filter_Alpha` | 116 | 0.1 | 0.01–1 | EMA coefficient of the pressure filter. |
| `Fon_Stable_Slope` | 117 | 0.02 bar/s | 0.001–1 | Slope below which the pressure counts as stable (plateau detection). |
| `Fon_Max_Starts_Per_Hour` | 118 | 10 | 1–16 | Start-rate guard → starts/hour fault. |
| `Fon_Hand_Min_Pressure` | 119 | 1.7 bar | 0–100 | Lower bound of the hand-valve pressure band (demand classification). |
| `Fon_Hand_Max_Pressure` | 120 | 2.0 bar | 0–100 | Upper bound of the hand-valve band. |
| `Fon_Tank_Min_Pressure` | 121 | 1.2 bar | 0–100 | Lower bound of the tank-draw band. |
| `Fon_Tank_Max_Pressure` | 122 | 1.6 bar | 0–100 | Upper bound of the tank-draw band. |
| `Fon_Hand_Max_Duration` | 123 | 20 min | 1–1440 | Longest plausible hand-valve event; longer → suspicious. |
| `Fon_Tank_Max_Duration` | 124 | 12 min | 1–1440 | Longest plausible tank-draw event. |
| `Fon_Unknown_Max_Duration` | 125 | 10 min | 1–1440 | Longest tolerated unclassified demand event. |
| `Fon_Flow_K_Hand` | 126 | 0 | 0–1000 | Flow k-factor for hand events (0 until bucket-test calibrated). |
| `Fon_Flow_K_Tank` | 127 | 0 | 0–1000 | Flow k-factor for tank events. |

### Network configuration (RW, NVS, ids 201–210)

| Datapoint | [id] | Default | Purpose |
|---|---|---|---|
| `Network_SSID` | 201 | from `network.json` | Wi-Fi SSID. |
| `Network_Password` | 202 | from `network.json` | Wi-Fi password. |
| `Network_Server` | 203 | from `network.json` | Server endpoint, IP or DNS name (resolved at connect time); empty → compile-time fallback. |
| `Network_Server_Port` | 204 | 8443 | Server WebSocket port. |
| `Network_DHCP` | 205 | 1 | 1 = DHCP (lease values are mirrored into the address points), 0 = static addressing. |
| `Network_IP_Address` | 206 | — | Static IP (or mirrored DHCP lease). |
| `Network_Subnetmask` | 207 | — | Static subnet mask (or mirrored lease). |
| `Network_Gateway` | 208 | — | Static gateway (or mirrored lease). |
| `Net_PS_Override` | 210 | 0 | Modem power-save policy: 0 auto (sleep in LOW, suspended while link POOR), 1 never sleep (max robustness), 2 always sleep in LOW (max saving). |

### Network commands & trial state (VOLATILE)

| Datapoint | Type | Purpose |
|---|---|---|
| `Network_Save` | U8 RW (0–4) | Command point, executed before the write batch, always reads 0: 1 = persist `Network_*`, 2 = persist + reboot, 3 = restore `Backup_*`, 4 = **trial reboot** — new config only kept if a server session comes up within 120 s, otherwise automatic rollback (see [Robustness.md](Robustness.md)). |
| `Network_Trial_State` | U8 RO | Trial result: 0 idle, 1 window running, 2 confirmed (kept), 3 rolled back. |

### Network backup (RO, NVS, ids 301–308 — written only by the system)

| Datapoint | [id] | Purpose |
|---|---|---|
| `Backup_DHCP` | 301 | Known-good copy of `Network_DHCP`. |
| `Backup_IP_Address` | 302 | Known-good static IP. |
| `Backup_Subnetmask` | 303 | Known-good subnet mask. |
| `Backup_Gateway` | 304 | Known-good gateway. |
| `Backup_Server` | 305 | Known-good server endpoint. |
| `Backup_Server_Port` | 306 | Known-good server port. |
| `Backup_SSID` | 307 | Known-good Wi-Fi SSID. |
| `Backup_Password` | 308 | Known-good Wi-Fi password. |

Captured by `network_config_backup()` after boot + Wi-Fi + server handshake
all succeeded; restored via `Network_Save = 3` or automatically by a failed
trial reboot.

### Logging (config RW NVS ids 401–403; status/command VOLATILE)

| Datapoint | [id] | Type | Purpose |
|---|---|---|---|
| `Log_Enabled` | 401 | BOOL (default 1) | Master switch of the structured logging system. |
| `Log_Runtime_Level` | 402 | U8 (default 3) | RAM-ring verbosity: 0 OFF, 1 ERROR, 2 WARN, 3 INFO, 4 DEBUG, 5 TRACE — remotely switchable. |
| `Log_Flash_Level` | 403 | U8 (default 2) | Verbosity of the persistent flash tier (`logstore` partition). |
| `Log_Next_Seq` | — | U32 RO | Next log sequence number (server uses it for incremental pulls). |
| `Log_Dropped` | — | U32 RO | Records dropped from the RAM ring (overflow). |
| `Log_Dropped_Flash` | — | U32 RO | Records dropped by the flash tier. |
| `Log_Prev_Boot_Available` | — | BOOL RO | 1 = a previous-boot log exists and can be pulled (`log_read_prev`). |
| `Log_Command` | — | U8 RW (0–3) | Command point, always reads 0: 1 = clear runtime ring, 2 = acknowledge previous-boot log, 3 = force flush. |

---

## 5. Relation to the legacy `data_store`

`core/data_store` is an older observer-pattern store (fixed key enum,
scaled-int values) that predates the datapoint engine. It is still used for
intra-firmware observer subscriptions but overlaps with datapoints;
new state belongs in `dp_list.def`, not in `data_store`.
