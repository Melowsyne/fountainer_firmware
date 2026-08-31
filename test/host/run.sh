#!/usr/bin/env bash
# Host test of the pump state machine (src/device/pump_manager.c) — without
# ESP-IDF, gcc only. pump_manager is PURE (no I/O, no FreeRTOS): pressure/
# sensor status/time go in as parameters, relay/state/metrics come out —
# so NO mocks are needed. Verifies the legacy guarantees (minimum times,
# dry run, no start above OFF, AUTO hysteresis, max runtime) plus the new
# behaviour (drop confirmation, recovering/lockout, demand classification,
# leak suspicion, starts-per-hour guard, fault ack).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ESP="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/test_pump_manager"

gcc -Wall -Wextra \
    -I "$ESP/src/device" \
    "$HERE/test_pump_manager.c" "$ESP/src/device/pump_manager.c" \
    -lm -o "$OUT"

"$OUT"

# Link score (Link_Robustness_v1): also PURE — RSSI/counters in,
# score/hysteresis out; tests base curve, penalty caps, EMA and POOR/GOOD.
OUT2="${TMPDIR:-/tmp}/test_link_score"
gcc -Wall -Wextra \
    -I "$ESP/src/network" \
    "$HERE/test_link_score.c" "$ESP/src/network/link_score.c" \
    -o "$OUT2"

"$OUT2"

# Log byte ring (logger_datenstruktuer.md): also PURE — variable records,
# wrap markers, DROP_OLDEST eviction, iter_seek; 100k stress test against shadow model.
OUT3="${TMPDIR:-/tmp}/test_log_ring"
gcc -Wall -Wextra \
    -I "$ESP/src/components/logging/include" \
    "$HERE/test_log_ring.c" "$ESP/src/components/logging/src/log_ring.c" \
    -o "$OUT3"

"$OUT3"

# Pressure sample ring (drucksensor_datenstruktur.md): PURE — DROP_OLDEST,
# gapless seqs, non-destructive since_seq cursor.
OUT4="${TMPDIR:-/tmp}/test_pressure_ring"
gcc -Wall -Wextra \
    -I "$ESP/src/components/pressure_history/include" \
    "$HERE/test_pressure_ring.c" "$ESP/src/components/pressure_history/src/pressure_ring.c" \
    -o "$OUT4"

"$OUT4"

# Datapoints (dp_write_batch: validation, atomicity, concurrency). Needs
# cJSON from the toolchain + mocks -> separate runner.
bash "$HERE/run_datapoints.sh"
