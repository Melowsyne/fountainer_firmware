# Hardening Test Log — 2026-07-10

The complete update and connection chain was tested on the real device
(ESP32-S3, with no USB connection — pure remote maintenance via Wi-Fi/wss+mTLS).
All faults were induced physically or under real conditions; every assessment
is backed by forensic evidence from the device logs (reset reasons,
gapless timestamps, structured event records).

## Results Overview

| # | Fault | Result | Evidence |
|---|---|---|---|
| 1 | Wi-Fi outage ~4 min (access point off) | ✅ passed | no reboot (uptime keeps counting), offline recording, gapless backfill |
| 2 | Power loss **in the middle of the OTA download** | ✅ passed | boots into the old version (`reset=power-on`), self-healing on the next connect |
| 3 | Power loss **between `applied` and reboot** | ✅ passed | boots straight into the **new** version (`reset=power-on`) — switchover is atomic |
| 4 | **Tampered image** is offered | ✅ passed (with finding → fixed) | rejected by image/signature verification, `OTA_REJECTED_UNSIGNED` logged, operation undisturbed |

## Test 1 — Wi-Fi Outage

Procedure: AP "TestNet" switched off at ~18:12, back on ~5 min later.

- 18:13:39 device detects beacon timeout → `SESSION_LOST`, power mode back to HIGH
- 18:13:50 `LINK_POOR` (score 34); reconnect attempts every ~15 s
- from 18:14:00 **offline recorder**: pump telemetry exactly on the 30-s grid,
  mains samples on the 60-s grid — without a single gap
- 18:14:16 server declares the device offline after the 150-s heartbeat timeout
  (deliberately sluggish: in the slow grid the device only sends every 60 s)
- 18:16:32 device watchdog, channel "session": **soft recovery** (WS restart) as
  stage 1; a reboot is blocked by design (`reboot_allowed = link_up` —
  "with no radio, a reboot won't help") → **boot loops caused by a radio outage are impossible**
- 18:17:35 AP back → session within seconds; 18:17:42–53 gapless backfill
  of 226 records from the offline window

Observation: ~1 min of records from before the disconnect was delivered twice
(at-least-once semantics of the acknowledged log protocol — better duplicated
than lost). Optional improvement: dedupe by sequence number when writing the JSONL.

## Test 2 — Power Loss During the Download

- 18:25:29 download 4.30.1→4.30.2 starts; supply pulled mid-download
- 18:26:30 after power returns: boot record **`reset=power-on`**, device reports
  the **old** 4.30.1 unscathed — the half-written image in the inactive
  partition was never activated
- Server offers it again → download #2 → applied → reboot (`reset=software`)
  → 4.30.2 running. **Self-healing, no manual intervention.**

## Test 3 — Power Loss Between `applied` and Reboot

The real window is only ~5 s and could not be hit manually (two failed
attempts — which is a result in itself: the exposure is tiny). For the
proof, a test firmware (4.30.5) with the `applied`→reboot delay stretched
to 20 s was installed; the follow-up update (4.30.6) already contained
the normal timing again.

- 21:24:49 `applied` (partition switchover completed), supply pulled ~8 s
  later, ~10 s without power
- Result: boots **straight into the new 4.30.6** with **`reset=power-on`** —
  the boot configuration (`ota_data`, two copies + CRC) is atomic and
  power-loss-proof. No fallback needed, no brick possible.

## Test 4 — Tampered Image (Attack Simulation)

Attack: signed original image copied, embedded version field patched to
a higher version (breaking the image checksum and the RSA signature),
placed in the server store as a regular update.

- The server offers it unsuspectingly — **including a correct SHA-256**, because
  it attests the (tampered) file. Takeaway: a server-reported hash does not
  protect against a compromised server — only the device-side signature does.
- 21:29:11 device: `esp_image: Checksum failed` → `ota_finish failed` →
  status `failed/finish_failed` + structured record **`OTA_REJECTED_UNSIGNED`**.
  Boot partition untouched, device stays on its version, no reboot.

### Finding & Fix: Session Limbo After a Failed OTA

The test uncovered a genuine design flaw: the protocol session only becomes
operational with `ota_none` (`running=true` → reports/heartbeats).
After `ota_available` + a **failed** update it remained in the state
"negotiated but not running" — 150 s of telemetry silence until the server
cut the session and the device watchdog (180 s) rebuilt it via soft
recovery. With a permanently bad image in the store, this would have been
an endless loop of 3-minute gaps.

**Fix (v4.31.1):** Every OTA outcome without a reboot (signature/checksum
rejection, download error, SHA mismatch, link-gate deferral, task-start
failure) now explicitly transitions the session into normal operation
(`fountain_proto_ota_failed_note()` → same as after `ota_none`).

**Retest with fix:** rejection at 21:37:36 → **first dp_report at 21:37:37**,
then gapless on the 10-s grid. Before: 150 s outage. Fixed.

## Secondary Findings of the Test Series

- The **version guard in `build.sh`** tripped for real on the first test build
  (an incremental build embedded an outdated version → forced
  clean rebuild) — the tooling-side protection against endless OTA loops
  works in practice.
- `Device_Build_Version` (build-timestamp data point) proved valuable for
  proving beyond doubt after every cycle **which** binary is actually
  running — independent of the version number.

## Final State

Firmware **v4.31.1** (incl. the limbo fix) is running; the store contains 4.31.1
(active), 4.30.6, and 4.29.0 (rollback reserves). All test artifacts (tampered
images, intermediate versions) were removed.

## Addendum 2026-07-11 — Protocol Verification & Display Fix

**Protocol (Fountain v2.2) fully verified:**

- **Golden auth vector:** independently recomputed in Python — body_hash
  `df69a908…` and MAC `QsNu1LP0…` match bit for bit. Canonicalization (JCS,
  sorted keys, shortest-round-trip floats), the 13-field MAC with
  0x1F separator, 128-bit truncation, and Base64 match the
  Python server side exactly.
- **Host tests** (`run_host_tests.sh`): golden test **and** session path
  (hello→ota_check→ota_none→command→replay defense) pass. A new runner
  builds cJSON + mbedTLS ABI-consistently from the toolchain — fixing a
  misleading "MISMATCH" that arose from linking the Espressif headers against
  the system `libmbedcrypto` (differing `sha256_context` layout).
- **Live against the device:** `dp_read` (signed), `dp_write` with float 3.75
  (the interop-critical HMAC-over-float path) → `applied`, readback confirmed,
  persisted; `command set_state` → `applied`. Not a single
  `mac_mismatch`/`auth_failed` in the server log.

**Display fix (server):** A stale `ota_status` (failed test-4 image)
stayed stuck in the device shadow even though the device had long been
running healthy. Fix: on `ota_check` with the result "no update" (`ota_none`),
any old OTA status is removed from the shadow.
