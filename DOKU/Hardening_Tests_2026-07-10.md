# Hardening Test Log — 2026-07-10

The complete update and connection chain was tested on the real device
(ESP32-S3, without USB connection — pure remote maintenance via Wi-Fi/wss+mTLS).
All faults were induced physically or for real; every assessment
is based on forensic evidence from the device logs (reset reasons,
gapless timestamps, structured event records).

## Results overview

| # | Fault | Result | Evidence |
|---|---|---|---|
| 1 | Wi-Fi outage ~4 min (access point off) | ✅ passed | no reboot (uptime keeps counting), offline recording, gapless back-delivery |
| 2 | Power failure **in the middle of the OTA download** | ✅ passed | boot on old version (`reset=power-on`), self-healing on next connect |
| 3 | Power failure **between `applied` and reboot** | ✅ passed | boot directly into the **new** version (`reset=power-on`) — switchover atomic |
| 4 | **Tampered image** is offered | ✅ passed (with finding → fixed) | rejection by image/signature check, `OTA_REJECTED_UNSIGNED` logged, operation undisturbed |

## Test 1 — Wi-Fi outage

Procedure: AP "TestNet" switched off at ~18:12, back on ~5 min later.

- 18:13:39 device detects beacon timeout → `SESSION_LOST`, power mode back to HIGH
- 18:13:50 `LINK_POOR` (score 34); reconnect attempts every ~15 s
- from 18:14:00 **offline recorder**: pump telemetry exactly on a 30 s grid,
  network samples on a 60 s grid — without a single hole
- 18:14:16 server declares the device offline after 150 s heartbeat timeout
  (deliberately sluggish: in the slow grid the device only sends every 60 s)
- 18:16:32 device watchdog channel "session": **soft recovery** (WS restart) as
  stage 1; a reboot is blocked by design (`reboot_allowed = link_up` —
  "without radio, a restart does not help") → **boot loops due to radio loss impossible**
- 18:17:35 AP back → session within seconds; 18:17:42–53 gapless
  back-delivery of 226 records from the offline window

Observation: ~1 min of records from before the drop was delivered twice
(at-least-once semantics of the acknowledged log protocol — better twice than
lost). Optional improvement: dedupe by sequence number when writing JSONL.

## Test 2 — Power failure during download

- 18:25:29 download 4.30.1→4.30.2 starts; supply pulled in the middle of the download
- 18:26:30 after power returns: boot record **`reset=power-on`**, device reports
  the **old** 4.30.1 intact — the half-written image in the inactive
  partition was never activated
- server offers again → download #2 → applied → reboot (`reset=software`)
  → 4.30.2 running. **Self-healing, no manual intervention.**

## Test 3 — Power failure between `applied` and reboot

The real window is only ~5 s and could not be hit manually (two
failed attempts — which is itself a result: the exposure is tiny). For
the proof, a test firmware (4.30.5) with the `applied`→reboot delay stretched to 20 s
was installed; the follow-up update (4.30.6) already contained
the normal timing again.

- 21:24:49 `applied` (partition switchover completed), supply pulled ~8 s later,
  ~10 s without power
- Result: boot **directly into the new 4.30.6** with **`reset=power-on`** —
  the boot configuration (`ota_data`, two copies + CRC) is atomic and
  power-failure-safe. No fallback needed, no brick possible.

## Test 4 — Tampered image (attack simulation)

Attack: signed original image copied, embedded version field patched to
a higher version (breaks image checksum and RSA signature),
placed in the server store as a regular update.

- The server offers it unsuspectingly — **including the correct SHA-256**, because it
  attests the (tampered) file. Lesson: a server-reported hash
  does not protect against a compromised server — only the device-side signature does.
- 21:29:11 device: `esp_image: Checksum failed` → `ota_finish failed` →
  status `failed/finish_failed` + structured record **`OTA_REJECTED_UNSIGNED`**.
  Boot partition untouched, device stays on its version, no reboot.

### Finding & fix: session limbo after failed OTA

The test uncovered a genuine design flaw: The protocol session only becomes
operational with `ota_none` (`running=true` → reports/heartbeats).
After `ota_available` + a **failed** update it remained in the state
"negotiated but not running" — 150 s of telemetry silence until the server
cut the session and the device watchdog (180 s) rebuilt it via soft recovery.
With a permanently bad image in the store this would have been an endless
loop of 3-minute holes.

**Fix (v4.31.1):** Every OTA outcome without a reboot (signature/checksum
rejection, download error, SHA mismatch, link-gate deferral, task-start
error) now explicitly transitions the session into normal operation
(`fountain_proto_ota_failed_note()` → as after `ota_none`).

**Retest with fix:** rejection 21:37:36 → **first dp_report 21:37:37**,
then gapless on a 10 s grid. Before: 150 s outage. Fixed.

## Side findings of the test series

- The **version guard in `build.sh`** triggered for real on the first test build
  (incremental build embedded a stale version → forced
  clean rebuild) — the tooling-side protection against endless OTA loops
  works in practice.
- `Device_Build_Version` (build timestamp datapoint) proved
  valuable for establishing beyond doubt after every cycle **which** binary
  is actually running — independent of the version number.

## Final state

Firmware **v4.31.1** (incl. limbo fix) is running; the store contains 4.31.1 (active),
4.30.6 and 4.29.0 (rollback reserves). All test artefacts (tampered
images, intermediate versions) have been removed.

## Addendum 2026-07-11 — Protocol verification & display fix

**Protocol (Fountain v2.2) fully verified:**

- **Golden auth vector:** independently recomputed in Python — body_hash
  `df69a908…` and MAC `QsNu1LP0…` match bit for bit. Canonicalisation (JCS,
  sorted keys, shortest-round-trip floats), 13-field MAC with
  0x1F separator, 128-bit truncation and Base64 correspond exactly to the
  Python server side.
- **Host tests** (`run_host_tests.sh`): golden test **and** session path
  (hello→ota_check→ota_none→command→replay defence) pass. The new runner
  builds cJSON + mbedTLS ABI-consistently from the toolchain — fixes a
  misleading "MISMATCH" that arose when linking the Espressif headers against the
  system `libmbedcrypto` (differing `sha256_context` layout).
- **Live against the device:** `dp_read` (signed), `dp_write` with float 3.75
  (the interop-critical HMAC-over-float path) → `applied`, readback confirmed,
  persisted; `command set_state` → `applied`. Not a single
  `mac_mismatch`/`auth_failed` in the server log.

**Display fix (server):** A stale `ota_status` (failed
test-4 image) stuck in the device shadow although the device had long been running
healthily. Fix: On `ota_check` with result "no update" (`ota_none`), any
old OTA status is removed from the shadow.
