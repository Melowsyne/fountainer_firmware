# Development Guide — Toolchain, Build, Versioning, Release, Tests

## 1. Prerequisites

- **PlatformIO** with the `espressif32` platform (the project pins
  ESP-IDF 5.5 via `framework = espidf`).
- Python 3 (build scripts under `tools/` run as PlatformIO extra scripts).
- Board: ESP32-S3 DevKitC-1, 16 MB flash, connected via USB
  (`/dev/ttyACM0`, USB-Serial-JTAG console).
- For signed builds: the CA directory checked out **next to** the repo
  (`../DO_NOT_COMMIT/CA`), containing `ota_signing/ota_signing_key.pem`. Without it the
  build completes unsigned with a warning (bench-only image).

## 2. Fresh checkout setup

Two inputs are git-ignored and machine-specific:

1. **`src/network/network.json`** — Wi-Fi SSID/password, server host/port.
   Created from the `.example` template automatically on the first build by
   `tools/ensure_network_json.py`; edit it afterwards. It is embedded as the
   provisioning default (`network_json_gen.c`) and can be overridden at
   runtime via the `Network_*` datapoints — no reflash needed to repoint a
   device.
2. **`src/network/certs/`** — `ca.crt.pem`, `client.crt.pem`,
   `client.key.pem`, copied from the CA (see
   [Certificates.md](Certificates.md)). The same script embeds them as
   `certs_gen.c`. Missing certs → the firmware falls back to plaintext
   `ws://`/`http://` (bench mode).

`EMBED_TXTFILES` is deliberately not used for either — it conflicts with the
PlatformIO build wrapper; the generator-script approach is the supported
path.

## 3. Build environments & scripts

`platformio.ini` defines three envs:

| Env | Use |
|---|---|
| `esp32s3` | USB development: flash at 460800 baud, monitor at 115200 |
| `esp32s3_ota` | OTA artifact build; `upload_command` delegates to `flash.sh ota` |
| `esp32s3_prod` | series-production build (no embedded per-device client material; driven by `tools/build_release.py`) |

Extra scripts (run automatically):

| Script | Phase | Purpose |
|---|---|---|
| `tools/lint_datapoints.py` | pre | validates `dp_list.def`, guards retired ids, exports `datapoints_meta.json` for the server UI |
| `tools/ensure_network_json.py` | pre | creates `network.json` from template; generates `network_json_gen.c` + `certs_gen.c` |
| `tools/gen_build_info.py` | pre | writes `src/main/build_info_gen.h` (`BUILD_TIMESTAMP_MS` → `Device_Build_Version` datapoint) |
| `tools/sign_firmware.py` | post | appends the RSA-3072 Secure-Boot-v2 signature block (`espsecure.py sign_data --version 2`); PlatformIO bypasses IDF's built-in signer, hence the explicit post-step |

Day-to-day entry points:

```bash
./build.sh        # clean-safe signed build -> binary/fountain-<version>.bin
./flash.sh        # USB flash + serial monitor (first bring-up / recovery)
./OTA_update.sh binary/fountain-<version>.bin   # copy to the server's FIRMWARE_UPDATES/
./clean.sh        # full build-tree clean
```

## 4. Versioning — and the stale-build trap

`version.txt` (plain `MAJOR.MINOR.PATCH`) → CMake `PROJECT_VER` →
`esp_app_desc_t.version` embedded at byte offset 48 of the image. The server
compares this string to decide whether to offer an update.

**Trap:** `PROJECT_VER` is read at CMake *configure* time. An incremental
PlatformIO build after editing `version.txt` may ship the **old** version
string — the server then re-offers the same update forever (OTA loop).
Guards in place:

- the root `CMakeLists.txt` fails the build on an empty version and declares
  `CMAKE_CONFIGURE_DEPENDS` on `version.txt`;
- `build.sh` / `flash.sh` read the version **actually embedded in the built
  binary** (`embedded_version()`, app-descriptor offset 48) and force a clean
  rebuild on mismatch — this is the authoritative check;
- `OTA_update.sh` re-derives the version from the binary itself (magic byte
  `0xE9` sanity check) when naming `fountain-<version>.bin`.

Release flow: bump `version.txt` → `./build.sh` → `./OTA_update.sh
binary/fountain-<new>.bin` → the server's `FirmwareStore` picks the
semantically newest image and offers it on each device's next `ota_check`.
To push a live device immediately, send it a signed `reboot` command — it
re-checks on reconnect.

## 5. Host tests (no hardware, no ESP-IDF)

Two suites run on a plain Linux host with `gcc`:

```bash
test/host/run.sh
```

builds and runs, directly against the real sources (no copies, no mocks —
both modules are *pure*):

- `test_pump_manager.c` — the pump state machine
  (`src/device/pump_manager.c`): legacy guarantees (minimum on/off times,
  dry-run detection, no start above OFF pressure, AUTO hysteresis, max
  runtime) plus demand classification, leak suspicion, drop confirmation,
  recovering/lockout, starts-per-hour guard, and fault acknowledgement.
- `test_link_score.c` — the link scorer (`src/network/link_score.c`): RSSI
  base curve, penalty caps, EMA, POOR/GOOD hysteresis.

```bash
cd src/components/clientside_protocol && ./test/host/run_host_tests.sh
```

builds cJSON + mbedTLS ABI-consistently from the toolchain and runs:

- `test_auth_golden.c` — the HMAC implementation against the golden vector
  from the server's AUTH-CONTRACT (byte-for-byte cross-language check);
- `test_session.c` — the full session path (hello → hello_ack → ota_check →
  ota_none → command/command_result) with an injected transport.

Design rule behind this: anything with domain logic is kept **pure**
(pump_manager, link_score, fp_session, fp_auth, fp_envelope) so it is
host-testable; I/O lives in thin binding modules (pump_task, link_quality,
fp_ws, fp_task).

## 6. Conventions

- Firmware state that the server may see or set is a datapoint — extend
  `dp_list.def`, never invent side channels.
- Cross-task communication goes through the event manager
  (`system_events.h` is the vocabulary); never call across task boundaries
  directly.
- New periodic work: register in `main/task_table.c` (task_manager),
  supervise via `main/watchdog_table.c` if it must not stall.
- Reboots go through `system_reboot_deferred` only (clean Wi-Fi teardown is
  wired as the pre-reboot hook).
- All source files carry the MIT/SPDX copyright header; comments and
  documentation are written in English.
