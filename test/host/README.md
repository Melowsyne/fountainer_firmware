# Host Tests (fountainer_firmware)

Logic modules that do not depend on real hardware are tested on a normal
Linux host — **without ESP-IDF, using plain `gcc`**. Both modules under test
are *pure* (no I/O, no FreeRTOS, no datapoint access): inputs go in as
parameters, outputs come back as return values — so **no mocks are needed**
and the real source files are compiled directly.

## `test_pump_manager.c` — pump state machine

Tests the real `src/device/pump_manager.c`. A fake clock and fake pressure
values drive the state machine through the safety-relevant scenarios:

- legacy guarantees: minimum on/off times, dry-run detection (no pressure
  rise → fault), no start above the OFF threshold, AUTO hysteresis
  (start below `Fon_Min_Pressure`, stop at `Fon_Max_Pressure`), maximum
  runtime cutoff;
- newer behavior: pressure-drop confirmation, recovering/lockout handling,
  demand classification (hand/tank/leak-suspect), leak suspicion,
  starts-per-hour guard, fault acknowledgement.

Each step asserts the internal state **and** the relay output **and** the
reportable `Fon_Current_State`.

## `test_link_score.c` — link scorer

Tests the real `src/network/link_score.c` (Link_Robustness_v1): RSSI base
curve, penalty caps, EMA smoothing, and the POOR/GOOD hysteresis (40/55).

## Build & run

```bash
test/host/run.sh
```

The script compiles both tests directly against the real sources
(`-I src/device` / `-I src/network`, no copies, no mock include path) and
runs them; it exits non-zero on the first failure.

Protocol host tests (golden HMAC vector, full session path) live separately
in the component:

```bash
cd src/components/clientside_protocol && ./test/host/run_host_tests.sh
```
