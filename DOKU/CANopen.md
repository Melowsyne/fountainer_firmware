# CANopen Slave — Object Dictionary, PDOs, Master Access

The firmware exposes its complete datapoint catalog to a CANopen master
(CiA 301 subset) over the TJA1051T/3 transceiver of the 24 V PCB (HW2.x).
The devkit (HW1.x) has no transceiver: `canopen_init()` succeeds, `Can_State`
stays 0 and the task is a no-op.

Companion master (Linux/SocketCAN, Raspberry Pi):
`fountainer_can_master_linux`.

## 1. Architecture

| Layer | File | Role |
|---|---|---|
| Framework component | `src/components/canopen/` (`co_core.[ch]`) | pure CiA-301 core: NMT slave, heartbeat, SDO server (expedited + segmented), 4 TPDO / 2 RPDO with dynamic mapping, SYNC consumer, EMCY producer. No IDF/FreeRTOS dependency; CAN driver and object dictionary injected via callbacks. Host-tested (`test/host/test_co_core.c`, 139 checks). |
| Board binding | `src/device/canopen_task.[ch]` | TWAI driver (legacy `driver/twai.h`), pin map from `hal_pins()`, datapoints as object dictionary, SDO writes through `task_com_apply_dp_write()`, EMCY on pump faults, bus-off recovery, `Can_*` status datapoints. |
| Task | `TM_TASK_CANOPEN` (`task_table.c`) | 10 ms cycle on core 0, prio 5: drain RX queue → `co_rx()`, `co_tick()`; watchdog channel `WD_CH_CANOPEN` (5 s, recover = driver restart, never reboots). |
| Catalog export | `tools/gen_canopen_eds.py` (build pre-script) | generates `DOKU/canopen/fountainer.eds` + `fountainer_od.json` from `dp_list.def`, mirrored into the master project. |

Pins (HW2.x, `hal.c`): CAN_TX = IO5 → TJA1051 TXD, CAN_RX = IO4 ← RXD,
CAN_PWDN = IO21 → S (silent/standby: HIGH at boot, LOW once the driver
runs — `hal_can_standby_set()`). Termination 120 Ω via DIP switch S3.

## 2. Configuration datapoints

| Datapoint | Type | Default | Meaning |
|---|---|---|---|
| `Can_Enabled` | BOOL RW NVS | 1 | master switch; 0 stops the driver, transceiver back to silent |
| `Can_Node_Id` | U8 RW NVS | 3 | CANopen node id 1..127 (all COB-IDs derive from it) |
| `Can_Bitrate` | U16 RW NVS | 250 | kbit/s: 10, 20, 50, 100, 125, 250, 500, 800, 1000 (`dp_constraints_ok`) |
| `Can_Heartbeat_Ms` | U16 RW NVS | 1000 | heartbeat producer period, 0 = off (also object 0x1017) |
| `Can_Loopback` | BOOL RW VOLATILE | 0 | bench self-test: TWAI no-ACK mode — own frames complete without a partner; a healthy transceiver shows `Can_Tx_Frames` rising with `Can_Bus_Errors` flat |
| `Can_Master_Timeout_Ms` | U16 RW NVS | 5000 | master presence window: a frame addressed to this node (NMT, SYNC, SDO request, RPDO, RTR) within this time keeps `Can_Master_Active` = 1 |

Node id / bitrate / enable / loopback are re-applied within 1 s (driver
restart); heartbeat immediately. Writes can come from the cloud, the local
maintenance access or the CAN master itself (SDO).

Status (RO, VOLATILE, all counters since boot and across driver restarts):

| Datapoint | Meaning |
|---|---|
| `Can_State` | 0 off/no hardware, 1 initialising, 2 pre-operational, 3 operational, 4 stopped, 5 bus-off or paused |
| `Can_Nmt_State` | raw CiA-301 NMT state as sent in the heartbeat: 0, 4 stopped, 5 operational, 127 pre-operational |
| **`Can_Master_Active`** | 1 while a master talks to this node (see `Can_Master_Timeout_Ms`); edge-logged as `LOG_EVT_CAN_MASTER` ("CAN master active" / "CAN master lost", WARN) |
| `Can_Master_Age_S` | seconds since the last master frame; 0xFFFFFFFF = never |
| `Can_Rx_Frames` / `Can_Tx_Frames` | frames received / sent |
| `Can_Sdo_Count` | SDO requests served |
| `Can_Pdo_Tx_Count` / `Can_Pdo_Rx_Count` | TPDOs sent / RPDOs applied |
| `Can_Emcy_Count` | emergencies sent |
| `Can_Bus_Errors` / `Can_Bus_Off_Count` | bus errors + failed transmissions / bus-off events |

Reading `Can_Master_Active` over CAN is itself a master frame, so after an
idle period the first read returns the stale 0 and the next one 1 (the
mirrors refresh once per second).

**Bus absent backoff:** every own frame on an unconnected or unterminated
bus ends in bus-off (no ACK). After 5 consecutive bus-offs without a
single received frame the driver pauses for 30 s (`Can_State` = 5, one WARN
log record) and retries — instead of looping bus-off once per heartbeat.

## 3. Object dictionary

| Index | Object | Notes |
|---|---|---|
| 0x1000 | Device type | 0 (no CiA device profile) |
| 0x1001 | Error register | bit0 generic + bit7 device-specific while a pump fault is latched |
| 0x1005 | COB-ID SYNC | 0x80 (consumer) |
| 0x1008 / 0x1009 / 0x100A | device name / hw rev / sw version | `"Fountainer"`, factory `hw_rev`, `version.txt` |
| 0x1014 | COB-ID EMCY | 0x80 + node id |
| 0x1017 | Producer heartbeat time | rw, mirrors `Can_Heartbeat_Ms` (RAM only) |
| 0x1018 | Identity | vendor 0 (no CiA id), product `0x464E5400` ("FNT"), revision `major<<16|minor<<8|patch`, serial = low 32 bit of `Device_Serial_Number` |
| 0x1200 | SDO server | 0x600+id → 0x580+id |
| 0x1400–0x1401 / 0x1600–0x1601 | RPDO1–2 comm / mapping | rw |
| 0x1800–0x1803 / 0x1A00–0x1A03 | TPDO1–4 comm / mapping | rw: COB-ID (bit 31 = disabled), transmission type, inhibit time (0.1 ms), event timer (ms) |
| **0x2000 + i** | **datapoint i** (order of `dp_list.def`), sub 0 | type from the catalog (BOOL→BOOLEAN, U8/ENUM→UNSIGNED8, F32→REAL32, STR→VISIBLE_STRING, U64→UNSIGNED64, …); access RO/RW/WO as in the catalog |
| 0x2FFF | Datapoint count | U16 |
| **0x3000 + i** | **descriptor record** of datapoint i | sub1 name (string), sub2 type code (`dp_type_t`), sub3 access (0 RO, 1 RW, 2 WO) — a master can build its object dictionary from the device alone, without an EDS |

Because the indices follow the catalog order, the EDS belongs to a firmware
version (the EDS carries it in `[FileInfo]`/`0x100A`); the descriptor
records make the master independent of it.

`Network_Password` and `Backup_Password` are **write-only** on CAN (read →
abort 0x06010001); the bus is unauthenticated.

**SDO writes** use exactly the cloud/local `dp_write` path
(`task_com_apply_dp_write`): type check, min/max, cross-field constraints,
NVS persist, `Network_Save`/`Log_Command` interception, `EVT_DP_WRITTEN`.
Rejections map to abort codes: out of range → 0x06090030, wrong length →
0x06070010/12/13, read-only → 0x06010002, constraint violation →
0x06040043, unknown → 0x06020000. Every write is logged
(`LOG_EVT_CAN_SDO_WRITE`: index, sub, abort code, length, name).

## 3a. Controlling the pump over CAN

Every RW datapoint is writable by SDO (and RPDO when mapped). For pump
control the command point **`Fon_Pump_Command`** (U8, always reads 0) is
polled by the pump task and executed under its own rules (fault latch,
minimum on/off times, dry-run protection):

| Value | Action | Equivalent protocol command |
|---|---|---|
| 1 | pump On (manual, unlimited) | `set_state On` |
| 2 | pump Off | `set_state Off` |
| 3 | mode Auto | `set_state Auto` |
| 4 | mode Manual | `set_state Manual` |
| 5 | restart (off → on) | `restart` |

Related points: `Fon_Fault_Ack` = 1 clears a latched fault (only with a
healthy sensor), `Fon_Pressure_Manual` / `Fon_Pressure_Value` feed a
simulated pressure, `Fon_Min_Pressure` … `Fon_Max_On_Time` are the live
configuration (persisted in NVS). Result and state are visible in
`Fon_Current_State` (1 Off, 2 On, 3 Auto, 5 Fault), `Fon_Relay_Output`,
`Fon_Fault_Code` and immediately in TPDO1. Verified 2026-09-23 with the
master: Auto → state 3, Manual → 1, On → 2 with relay on and TPDO1
`183 [7] 00 00 20 40 02 01 00` (2.5 bar simulated, On, relay 1, fault 0),
Off respects `Fon_Min_On_Time`, sensor loss → fault + EMCY 0xFF01.

## 4. Process data (defaults)

Event-driven (transmission type 255): sent on value change (byte-exact,
respecting the inhibit time) and at least every event-timer period. Only
in NMT OPERATIONAL. A master may remap everything through 0x1A0x/0x180x;
the defaults return on "reset communication".

| PDO | COB-ID | Mapping (bytes) | Inhibit / event timer |
|---|---|---|---|
| TPDO1 | 0x180+id | `Fon_Current_Pressure` F32, `Fon_Current_State` U8, `Fon_Relay_Output` U8, `Fon_Fault_Code` U8 (7 B) | 200 ms / 1000 ms |
| TPDO2 | 0x280+id | `Fon_Pressure_Filtered` F32, `Fon_Pressure_Slope` F32 (8 B) | 500 ms / 2000 ms |
| TPDO3 | 0x380+id | `System_Uptime` U32, `Fon_Run_Time` U32 (8 B) | 1000 ms / 5000 ms |
| TPDO4 | 0x480+id | `System_Temperature` F32, `Net_Link_Score` U8, `Fon_Demand_State` U8, `Fon_Starts_Per_Hour` U8, `System_Power_Mode` U8 (8 B) | 1000 ms / 5000 ms |
| RPDO1 | 0x200+id | `Fon_Fault_Ack` U8, `Fon_Event_Label` U8 | immediate (255) |
| RPDO2 | 0x300+id | disabled | — |

Transmission types 0..240 (SYNC-driven) and RTR on TPDO COB-IDs are
supported as well. RPDO data go through the same validated `dp_write`
path; a too-short RPDO is rejected as a whole with EMCY 0x8210.

**EMCY** (0x80+id): `EVT_PUMP_FAULT` → error code `0xFF00 | fault code`,
error register 0x81, manufacturer bytes `[fault code, pump state, 0, 0, 0]`;
`EVT_PUMP_FAULT_CLEARED` → code 0x0000 (error reset). NMT "reset node"
re-initialises the CANopen stack only (no device reboot).

## 5. Master quick start (Raspberry Pi PI_HOST, MCP2515 HAT)

```bash
sudo dtparam spi=on && sudo dtoverlay mcp2515-can0,oscillator=16000000,interrupt=25
sudo ip link set can0 up type can bitrate 250000 restart-ms 100
candump can0                      # 0x703 heartbeat every second once wired
~/can-venv/bin/fcm scan           # node ids on the bus
~/can-venv/bin/fcm info
~/can-venv/bin/fcm read Fon_Current_Pressure Fon_Current_State
~/can-venv/bin/fcm write Fon_Max_Pressure 3.8
~/can-venv/bin/fcm monitor        # NMT start + live TPDOs / EMCY
```

Log module `LOG_MOD_CANOPEN` (9), events 900–908 (`logging.h`).

## 5a. Protocol timing and throughput (measured 2026-09-23)

Setup: FNT-000003 (ESP32-S3, TWAI) ↔ PI_HOST (Raspberry Pi 3B, MCP2515 on
SPI, python-canopen), 250 kbit/s, 2 nodes, both ends terminated,
`fcm bench` (round-robin SDO reads, TPDOs running, NMT operational).

| Test | Result |
|---|---|
| Frame on the wire (scope) | 8-byte SDO frame 420 µs ≈ 105 bits at 250 kbit/s, bit time 3.9–4.0 µs, levels CANH 2.6→3.8 V / CANL 2.6→1.4 V |
| SDO read, 1 datapoint (F32), 20 s | **521 reads/s**, latency min 1.7 / avg 1.9 / p95 2.2 / max 6.9 ms |
| SDO read, 8 datapoints incl. one segmented string, 30 s | **457 reads/s**, avg 2.2 ms, p95 3.8 ms, max 11.9 ms, 0 errors |
| SDO read + interleaved SDO write (`Fon_Event_Label`, NVS path), 30 s | writes avg 35 ms (dp_write incl. NVS save), reads 8 ms — before the event-driven RX |
| Segmented string read (`Device_SW_Version`, `Network_SSID`) | 2 round trips per read |
| TPDOs in parallel | 2–3.5 frames/s (event timers 1/2/5 s, change-driven) |
| Bus load during 60 s stress (12 400 frames, 99 kB) | ≈ 23 kbit/s ≈ 9 % of 250 kbit/s |
| Errors | `Can_Bus_Errors` 0 over > 26 000 SDO exchanges; master presence stayed active |

Before the event-driven RX loop (task polled every 10 ms) the SDO latency
was exactly one cycle: 100 reads/s at 10.0 ms; the CAN task now blocks in
`twai_receive` for the whole period and wakes on every frame, so the
latency is the processing time (~1.7 ms round trip incl. the MCP2515/SPI
side on the Pi). SDO writes cost ~35 ms because the validated dp_write path
persists the config region to NVS after every batch.

## 6. Bring-up record (2026-09-22, FNT-000003 on PI_HOST)

- Firmware 4.40.0 flashed over USB via PI_HOST; boot log:
  `[canopen] node 3 @ 250 kbit/s, heartbeat 1000 ms (tx IO5 rx IO4)`,
  task `canopen` running; `Can_*` datapoints visible in the server UI.
- ESP32 self-test (`Can_Loopback=1`, no-ACK mode): `Can_Tx_Frames` rises
  ~1/s, `Can_Bus_Errors` flat → transceiver powered, in normal mode, TX/RX
  pins correct.
- Pi module ("MCP2515 CAN Module", MCP2515 + MCP2562, 16 MHz crystal, P1 =
  120 Ω termination) on SPI0 CE0 / INT GPIO25: driver and overlay verified
  after a reboot (`dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=25`,
  unit `fountainer-can0`); its frames measured on the scope have the correct
  4 µs bit time at 250 kbit/s and clean differential levels.
- **Root cause (scope + multimeter):** with the Fountainer powered, both bus
  wires idled at ~4.1 V against Pi ground (2.5 V expected), independent of
  the transceiver mode (normal/silent); IC4 pins 6/7 measured 11 V unloaded
  and **IC4 pin 3 (VCC) carries 24 V instead of 5 V** — the TJA1051T/3
  (abs. max 7 V) is destroyed. The whole **+5 V rail sits at 24 V** (24 V
  measured at FL2 and L2 as well): the LT1934 buck converter's switch is
  shorted — FB pin reads 5.68 V (divider intact, 24 V × 332k/1332k), yet
  the output stays at VIN. TP2 showed 5 V in earlier bring-up tests, so the
  converter failed later. Not a firmware or master issue. Repair: replace
  the LT1934 (check VIN–SW for an external bridge), verify 5.0 V / FB 1.25 V,
  then replace IC4; J2 sensor supply carried 24 V too. Rev. 2 idea: protect
  IC4/J2 against a converter failure (fuse/clamp on the 5 V rail). After
  that `candump can0` must show `703 [1] 7F` every second.
- **Repair verified (2026-09-22 20:13, IC2 + IC4 replaced, supply 20 V):** TP2 5.0 V;
  bus idle 2.56 / 2.61 V, frames 4 µs bit time (scope); `candump` shows `703 [1] 7F`
  every second; `fcm scan/info/read/write/pdo/monitor` all work: identity 0x464E5400 /
  4.40.0, SDO read of typed datapoints, SDO write `Can_Heartbeat_Ms` applied and
  observed on the bus, NMT start → TPDO1–4 flowing with live values,
  `Can_Bus_Errors` 0. Root-cause analysis of the converter failure:
  `fountainer_hw_24v/DOKU_2026-09-22_Ausfall_LT1934_Analyse.md`.
- **2026-09-23:** `Fon_Pump_Command`, master-presence datapoints, extended
  counters, event-driven RX; full functional round (identity, connection
  points, SDO writes incl. rejections, RPDO1, pump control, EMCY, master
  timeout) and the stress tests above passed; see §3a / §5a.
