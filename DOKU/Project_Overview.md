# Fountainer — Networked Well-Pump Controller on ESP32-S3

> An industrial-grade firmware/server platform, born from a real-world task:
> **controlling a well pump (1 kW, 8 m deep) safely, autonomously, and with full remote maintenance** —
> designed from the start as a reusable foundation for further IoT projects.

---

## 1. What This Is About

A deep well supplies house and garden. The submersible pump (1 kW, 8 m head) is
switched via a solid-state relay; the controlled variable is the **line pressure**, measured
with a 0.5–4.5 V pressure transmitter (0–500 PSI) on an external 16-bit ADC (ADS1115).
The firmware keeps the pressure within a configurable band, detects fault patterns such as
**dry run, leakage, pipe burst, and defective check valves**, classifies
draw events (hand valve vs. tank fill), and reports everything to a Linux server —
encrypted, signed, and offline-resilient.

The project consists of two repositories:

| Repo | Role | Technology |
|---|---|---|
| `fountainer_firmware` | Device firmware (this repo) | C, ESP-IDF 5.5, ESP32-S3, PlatformIO |
| `fountainer_server` | Operations server + web UI | Python (asyncio, websockets, aiohttp) |

**Firmware scope:** ~11,000 lines of C in cleanly separated modules, 107 datapoints,
host unit tests, signed OTA updates, mTLS communication.

---

## 2. Architecture at a Glance

```mermaid
flowchart TB
    subgraph HW["Hardware"]
        PS["Pressure sensor 0.5-4.5 V<br/>(ADS1115, I2C)"]
        SSR["SSR / pump 1 kW<br/>(LEDC PWM)"]
        DHT1["AM2302 climate (GPIO4)"]
    end

    subgraph DEVICE["src/device — device logic"]
        HAL["hal<br/>ADC burst, relay,<br/>chip temperature"]
        AM["onewire_am2302<br/>bit-banging driver,<br/>diagnostic telemetry"]
        PM["pump_manager<br/>PURE state machine<br/>(host-testable, 32 tests)"]
        PT["pump_task<br/>200 ms control cycle,<br/>sensor debounce"]
        TM["task_measure<br/>climate & chip sensors"]
        CMD["command<br/>RPC dispatcher"]
    end

    subgraph COMP["src/components — reusable building blocks"]
        DP["datapoints<br/>X-macro registry,<br/>per-key NVS"]
        EV["event_manager<br/>publish/subscribe"]
        LOG["logging<br/>RAM ring + flash tier,<br/>server pull"]
        WD["watchdog<br/>5 channels, RTC diagnostics,<br/>reboot brake"]
        TSK["task_manager<br/>task lifecycles"]
        PROTO["clientside_protocol<br/>Fountain v2.2: envelope,<br/>HMAC, session, WS"]
    end

    subgraph NET["src/network — connectivity"]
        WLAN["wlan_com"]
        COM["task_com<br/>protocol glue,<br/>command intercepts"]
        OTA["ota_task<br/>signed updates"]
        NCFG["network_config<br/>trial commit/rollback"]
        LINK["link_quality/score<br/>RSSI rating, adaptation"]
    end

    subgraph SRV["fountainer_server (Linux)"]
        WSS["wss + mTLS :8443"]
        FW["Firmware store :8080"]
        UI["Admin web UI :8010"]
    end

    PS --> HAL --> PT
    DHT1 --> AM --> TM
    PT --> PM --> PT
    PT --> SSR
    PT & TM --> DP
    DP <--> COM
    EV -.wires up all modules.- DEVICE & NET
    LOG --> COM
    WD -.monitors.- DEVICE & NET
    CMD <--> COM
    COM <--> PROTO <--> WLAN <--> WSS
    OTA <--> FW
    UI --- WSS
```

**Design principle:** The domain logic (`pump_manager`) is a *pure* C state machine
with no ESP dependency whatsoever — it runs unchanged in the host test runner on a PC.
All cross-cutting concerns (events, logging, watchdog, datapoints, protocol) are
standalone components, directly reusable in follow-up projects.

---

## 3. The Features in Detail

### 3.1 Datapoint Mechanics — One Line, Complete Behavior

Every device state and every setting is a **datapoint**: typed,
access-controlled (RO/RW), persistable (NVS), and bounded by limits. The entire
registry is a single X-macro file — **one new line generates storage, validation,
persistence, network access, and UI display in a single step**. Excerpt from
`src/components/datapoints/dp_list.def` (lines 100–142):

```c
DP(   Fon_Starts_Per_Hour,       U8,  RO, VOLATILE,   0, 0,       NAN,    NAN,    0     )   /*@ unit=/h @*/
/* Cumulative failed pressure readings since boot (I2C glitches etc.; a
 * sensor fault only latches after 3 CONSECUTIVE misses — debounced). */
DP(   Fon_Sensor_Err_Count,      U32, RO, VOLATILE,   0, 0,       NAN,    NAN,    0     )
/* Spread (max-min, mV) of the 4-sample ADC burst: sensor/wiring noise. */
DP(   Fon_Sensor_Noise_mV,       U16, RO, VOLATILE,   0, 0,       NAN,    NAN,    0     )   /*@ unit=mV @*/
/* Write 1 = acknowledge a latched pump fault (needs a healthy sensor);
 * polled by the pump task, always reads 0. */
DP(   Fon_Fault_Ack,             U8,  RW, VOLATILE,   0, 0,       0.0f,   1.0f,   0     )
/* Manual classification of the CURRENT/last event (0=unknown 1=none
 * 2=hand 3=tank 4=leak 5=break 6=sensor 7=maintenance); logged on change. */
DP(   Fon_Event_Label,           U8,  RW, VOLATILE,   0, 0,       0.0f,   7.0f,   0     )

/* --- Fountain configuration (RW, NVS) ----------------------------------- */
DP(   Fon_Min_Pressure,          F32, RW, NVS,      101, 2.0f,    0.0f,   10.0f,  0     )   /*@ unit=bar dec=2 @*/
DP(   Fon_Max_Pressure,          F32, RW, NVS,      102, 3.5f,    0.0f,   10.0f,  0     )   /*@ unit=bar dec=2 @*/
DP(   Fon_Alert_High_Pressure,   F32, RW, NVS,      103, 4.5f,    0.0f,   12.0f,  0     )   /*@ unit=bar dec=2 @*/
DP(   Fon_Alert_Low_Pressure,    F32, RW, NVS,      104, 0.3f,    0.0f,   10.0f,  0     )   /*@ unit=bar dec=2 @*/
DP(   Fon_Min_On_Time,           U16, RW, NVS,      105, 30,      0.0f,   3600.0f,0     )   /*@ unit=s @*/
DP(   Fon_Max_On_Time,           U16, RW, NVS,      106, 300,     10.0f,  65535.0f,0    )   /*@ unit=s @*/
DP(   Fon_Dry_Run_Detect_Time,   U16, RW, NVS,      107, 30,      1.0f,   3600.0f,0     )   /*@ unit=s @*/
/* ... */
/* --- Pump manager --------------------------------------------------------
 * Sensor full scale: keeps the original hardware value (0.5-4.5 V ->
 * 0-500 PSI = 34.47 bar) but is runtime-changeable per requirement; the
 * effective curve is bar = (V-0.5)/4.0 * Range * Fon_Sensor_Scale
 * + Fon_Sensor_Offset(mbar). Demand bands/durations classify hand valve
 * vs. tank draw; flow k-factors stay 0 until calibrated (bucket test). */
DP(   Fon_Sensor_Range_Bar,      F32, RW, NVS,      114, 34.47f,  0.1f,   100.0f, 0     )   /*@ unit=bar dec=2 @*/
DP(   Fon_Filter_Alpha,          F32, RW, NVS,      116, 0.1f,    0.01f,  1.0f,   0     )
DP(   Fon_Max_Starts_Per_Hour,   U8,  RW, NVS,      118, 10,      1.0f,   16.0f,  0     )
DP(   Fon_Hand_Min_Pressure,     F32, RW, NVS,      119, 1.7f,    0.0f,   100.0f, 0     )   /*@ unit=bar dec=2 @*/
DP(   Fon_Tank_Min_Pressure,     F32, RW, NVS,      121, 1.2f,    0.0f,   100.0f, 0     )   /*@ unit=bar dec=2 @*/
DP(   Fon_Hand_Max_Duration,     U16, RW, NVS,      123, 20,      1.0f,   1440.0f,0     )   /* min */   /*@ unit=min @*/
```

Core ideas of the mechanism:

- **Stable NVS IDs per point** ("per-key store"): Every persistent point has a fixed
  ID and is stored as its own NVS entry. Points can be reordered, added, or
  removed at will — **stored customer configuration survives every firmware update**,
  including an automatic one-time migration from the older blob format.
- **Deadband reporting:** F32 points only report on relevant change — the
  1-second change reporting stays lean even over weak radio links.
- **Build lint + UI metadata:** A pre-build script (`tools/lint_datapoints.py`) enforces
  naming, limit, and ID rules and exports units/decimal places/enum mappings
  as JSON — **the web UI generates its complete datapoint view from it**, with no
  duplicate maintenance.

### 3.2 The "Fountain v2.2" Communication Protocol — RPC, Telemetry, OTA in One

A custom, lean JSON protocol over **WebSocket Secure with mutual mTLS**
(private CA, device certificates). Every message is wrapped in an **HMAC-SHA256-signed
envelope** with canonical JSON serialization — tampering or replay is detected immediately.

The protocol combines three roles:

1. **Remote procedure calls:** The server invokes device functions (`command`:
   set_state/restart/reboot …, `dp_read`, `dp_write` with batch semantics) and receives
   type-checked, acknowledged responses (`command_result`, `mac_mismatch` protection).
2. **Telemetry:** state-driven `dp_report` messages (change reporting with
   deadband) plus a heartbeat with uptime/RSSI.
3. **Log transport & OTA signaling:** The server *pulls* logs in acknowledged
   batches (`log_read`/`log_batch`/`log_ack`), including retroactively from the time **before
   the last reboot**; on connection setup, `ota_check` looks for new firmware.

### 3.3 OTA Updates — Signed, Gated, Self-Healing

- Images are **RSA-3072-signed (Secure Boot V2)**; the device verifies the signature
  before switching the boot partition. Unsigned images are rejected and logged.
- The server offers updates only during session setup (`ota_check` → `ota_available`
  with SHA-256 and download URL); the device downloads via a separate firmware endpoint.
- A **link gate** defers updates while the radio link is poor, and a
  version guard in the build prevents images with a stale embedded version
  (the root cause of classic endless update loops — secured at the tooling level here).
- Progress and result flow back into the UI as `ota_status` telemetry.

```mermaid
sequenceDiagram
    participant G as Device (ESP32-S3)
    participant S as Server (wss, mTLS)
    participant F as Firmware store

    G->>S: hello (serial number, FW version, HMAC)
    S->>G: hello_ack (session established)
    G->>S: ota_check
    alt newer version available & link good
        S->>G: ota_available (version, SHA-256, URL, signed)
        G->>F: GET /firmware/fountain-x.y.z.bin
        F-->>G: signed image (RSA-3072)
        G->>G: verify signature, switch partition
        G->>S: ota_status: applied (100 %)
        G->>G: reboot into new firmware
        G->>S: hello (new version) → ota_none
    else up to date / link poor
        S->>G: ota_none (gate: update deferred)
    end
```

### 3.4 Backup & Trial Mechanism — Remote Configuration Without Cold Sweat

Network parameters (SSID, password, IP mode, server address) can be changed remotely
via datapoints — with a double safety net:

- **Backup set:** The most recent *working* configuration is kept as a dedicated
  `Backup_*` datapoint set in NVS.
- **Trial reboot:** New Wi-Fi credentials are first activated **on a trial basis**. If the
  device fails to establish a complete server session within **120 s**, it automatically
  rolls back to the old values and reports the failure — a typo in the Wi-Fi password
  can no longer strand the device down in the well shaft. An NVS state machine
  even survives power failures in the middle of the trial.

### 3.5 Event Management — Decoupled Modules

A central **publish/subscribe event manager** wires the modules together without them
knowing each other: `EVT_SYSTEM_BOOT`, `EVT_SESSION_READY/LOST`, `EVT_PUMP_FAULT`,
`EVT_NETWORK_CONFIG_RESTORED`, `EVT_LINK_POOR` … Whoever wants to react subscribes —
for example, the session-ready subscriber confirms the Wi-Fi trial and refreshes the
backup set. New reactions are added without touching the senders.

### 3.6 Logging — From RAM Ring to Pre-Boot Forensics

- **Two-tier:** a 32 KB RAM ring for current activity plus a 2×64 KB **flash tier** (behind the
  OTA slots, detected at runtime) for persistence across reboots.
- **Server pull instead of push:** The server fetches logs in acknowledged batches — nothing
  is lost, not even across connection drops; after a crash,
  `log_read_prev` delivers the records of the **previous boot**.
- **Offline recorder:** Without a session, the device keeps recording pump telemetry at a
  30 s interval and delivers the gap seamlessly afterwards — proven with a forced
  150 s radio blackout.
- **Structured records:** Stable event IDs + two arguments instead of text parsing —
  the same infrastructure carried complete **remote diagnoses** in this project (e.g.
  narrowing down a sensor wiring fault via handshake-phase telemetry,
  without ever connecting an oscilloscope).

### 3.7 Watchdog — Supervision With Memory

Five logical channels (session, measurement, monitor, events, pump) feed a
central watchdog. Highlights:

- The session channel counts only **successfully sent** frames — half-open
  TCP connections ("wedges") cannot masquerade as healthy.
- **RTC diagnostic memory:** On a watchdog reset, the culprit information survives the
  reboot and lands in the first log batch.
- **Reboot brake:** At most 3 consecutive watchdog restarts, after which the device
  enters a waiting safe state instead of a boot loop.

### 3.8 Link Robustness — Built for Poor Wi-Fi in the Shaft

An RSSI-based **link score** (EMA-smoothed, with POOR/GOOD hysteresis) drives
runtime adaptations: a throttled report interval, reduced log batch sizes,
Wi-Fi power-save control, and the OTA gate. All adaptations have been made
deterministically verifiable via a test command.

### 3.9 Pump Logic — Testable Like a Library Module

The state machine covers states from `IDLE` through `RUNNING` to `FAULT` and detects:
dry run (no pressure build-up), suspected leakage (cyclic re-pumping),
pipe burst (pressure drop under load), stuck check valves, sensor failure
(3× debounced), and excessive start frequency. Events are classified by pressure band and
duration as *hand valve* or *tank fill* and logged with a volume estimate.
**32 host unit tests** cover the transitions — runnable without hardware, in seconds.

---

## 4. Outlook: Datapoints as Training Data for a Tiny LLM

The logging pipeline is deliberately designed as a **data collection for machine learning**:
time-series records (`PM_SAMPLE`: filtered pressure, pressure gradient, state/demand)
and event summaries (`PM_EVENT`: duration, minimum pressure, volume), together
with the manual labels (`Fon_Event_Label`), form a growing, labeled
dataset of real pump behavior.

Planned extension: a **compact on-device model ("Tiny LLM"/TinyML) directly on the
ESP32-S3** (using the vector instructions of the S3 architecture) that learns from these
patterns to detect anomalies earlier than the rule-based logic — e.g. creeping leaks or
aging check valves. The datapoint `Fon_Anomaly_Score` is already reserved as its
output channel; the control loop stays deterministic, the model provides a
second opinion and early warning.

## 5. Reuse: A Platform, Not Just a Project

All components under `src/components/` are deliberately cut project-neutral
(own include directories, no dependency on the pump logic):

- `datapoints` — declarative state/configuration registry with persistence
- `clientside_protocol` — signed, session-based device link with RPC & OTA
- `event_manager`, `task_manager`, `logging`, `watchdog` — operational base kit

At its core, a new device (different sensors, different actuators) only needs: its own
`dp_list.def`, its own device drivers, its own domain state machine. Server, UI,
security, OTA, and diagnostics are ready to go.

---

## 6. Build & Deployment Workflow

The complete release cycle is two commands (all paths relative; the
repos sit next to each other as sibling directories):

```bash
./build.sh                                  # build + sign + verify
./OTA_update.sh binary/fountain-4.31.1.bin  # deploy to the server
```

- **`build.sh`** builds the OTA image (PlatformIO env `esp32s3_ota`), appends the
  RSA-3072 signature block (post-script), and stores the result, versioned,
  as `binary/fountain-<version>.bin`. Built in is the **version guard**:
  it reads the actually embedded version from the binary's app descriptor
  and forces a clean rebuild if it deviates from `version.txt` —
  the root cause of classic endless OTA loops is thereby ruled out at the
  tooling level.
- **`OTA_update.sh <file>`** checks the image magic and embedded version and
  copies the file to `../fountainer_server/FIRMWARE_UPDATES/` — the target
  file name is **derived from the embedded version**, so file name and
  reported version can never diverge. The server offers the
  image at the device's next `ota_check`.
- **`flash.sh`** remains for initial commissioning: USB flash + serial
  monitor (`./flash.sh`) or build+deploy in one step (`./flash.sh ota`).
- The build automatically runs: datapoint lint + UI metadata export,
  `network.json`/certificate embedding, build timestamp generation
  (`Device_Build_Version`), and the signing step.

For details on the PKI (structure, certificate reissuance, key rotation),
see [Certificates.md](Certificates.md).

---

## 7. Quality and Security Features (Short List)

| Area | Measure |
|---|---|
| Transport | WebSocket Secure, **mTLS** with a private CA, TLS session tickets |
| Messages | **HMAC-SHA256** envelopes, canonical JSON, replay protection |
| Firmware | **Secure Boot V2 / RSA-3072-signed OTA images** |
| Configuration | Trial commit with **automatic 120 s rollback**, backup set in NVS |
| Robustness | Watchdog channels with RTC forensics, reboot brake, offline recorder |
| Testability | Pure-C state machine with **32 host tests**, protocol **golden tests** (ABI-consistent host runner), **hardening/fault-injection test series** (see [Hardening_Tests_2026-07-10.md](Hardening_Tests_2026-07-10.md)) |
| Toolchain | Datapoint **lint in the build**, UI metadata export, version guard against OTA loops, build timestamp datapoint (`Device_Build_Version`) |
| Operations | ~30+ signed OTA cycles including passed fault-injection tests (radio loss, power failure during/after an update, tampered image), remote diagnosis purely via log telemetry |

---

## 8. Technical Key Data

- **MCU:** ESP32-S3 (dual-core Xtensa LX7, 16 MB flash), ESP-IDF 5.5, PlatformIO
- **Measurement chain:** ADS1115 (16-bit, 4-sample burst with trimmed mean against outliers),
  configurable sensor curve (range/scale/offset at runtime)
- **Climate sensing:** AM2302, pin-parameterized driver (multi-sensor capable) with
  remote wiring diagnostics (handshake phase, line idle level, raw frames)
- **Control cycle:** 200 ms, EMA pressure filter, spike filter, 3× sensor debounce
- **Server:** Python asyncio; device WSS (8443, mTLS), firmware HTTPS (8080),
  admin web UI (8010) with a live-generated datapoint view
- **Firmware code size:** ~11,000 lines of C, 107 datapoints (47 persistent)
