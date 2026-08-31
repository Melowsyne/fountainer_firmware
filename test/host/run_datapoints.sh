#!/usr/bin/env bash
# Host test for datapoints.c (dp_write_batch validation + atomicity +
# concurrency). datapoints.c is coupled to ESP-IDF (FreeRTOS mutex, NVS,
# esp_log/esp_system) -> we build against lightweight mocks (test/host/mocks/)
# and cJSON from the PlatformIO toolchain (like run_host_tests.sh of the
# clientside_protocol component). The DP mutex mock is a REAL pthread
# mutex so that the concurrency stress test checks real serialization.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ESP="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/test_datapoints"

# Find the cJSON source: prefer the system copy, else framework-espidf* (IDF 5.5.x).
CJDIR=""
CJSON_INC=()
CJSON_SRC=()
if [ -f /usr/include/cjson/cJSON.h ]; then
    CJSON_INC=(-I/usr/include/cjson)
    CJSON_LINK=(-lcjson)
else
    PIO_PKGS="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
    for d in "$PIO_PKGS"/framework-espidf*/components/json/cJSON; do
        [ -f "$d/cJSON.c" ] && CJDIR="$d" && break
    done
    [ -n "$CJDIR" ] || { echo "ERROR: cJSON source not found (IDF 5.5.x installed?)" >&2; exit 1; }
    echo "   cJSON: $CJDIR"
    CJSON_INC=(-I"$CJDIR")
    CJSON_SRC=("$CJDIR/cJSON.c")
    CJSON_LINK=()
fi

gcc -Wall -Wextra -pthread -O1 \
    -I "$HERE/mocks" \
    -I "$ESP/src/components/datapoints" \
    "${CJSON_INC[@]}" \
    "$HERE/test_datapoints.c" \
    "$ESP/src/components/datapoints/datapoints.c" \
    "${CJSON_SRC[@]}" \
    -lm "${CJSON_LINK[@]}" -o "$OUT"

"$OUT"
