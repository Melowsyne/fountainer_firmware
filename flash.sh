#!/bin/bash
set -e

PIO=~/.platformio/penv/bin/pio
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

# OTA deployment runs via the dedicated scripts (relative paths,
# local fountainer_server as sibling checkout):
#   ./build.sh + ./OTA_update.sh
VERSION=$(tr -d '[:space:]' < "$PROJECT_DIR/version.txt")

# ------------------------------------------------------------
# Version guard against the build trap:
# version.txt is only read at CMake configure time; an incremental
# PlatformIO build does NOT always pick up a changed version -> the .bin
# then reports an old version -> the server offers the same OTA endlessly.
# embedded_version() reads the actually embedded version directly from the
# app descriptor (esp_app_desc_t.version, char[32] @ offset 0x30 of the app image).
# build_versioned() builds, checks, and forces a clean rebuild on mismatch.
# ------------------------------------------------------------
embedded_version() {
    dd if="$1" bs=1 skip=48 count=32 2>/dev/null | tr -d '\0'
}

build_versioned() {
    local env="$1"
    local bin="$PROJECT_DIR/.pio/build/${env}/firmware.bin"

    echo "==> Building firmware (env=${env}, version.txt=v${VERSION})..."
    "$PIO" run -e "$env"

    if [ "$(embedded_version "$bin")" != "$VERSION" ]; then
        echo "!!  Embedded version ($(embedded_version "$bin")) != version.txt (${VERSION})."
        echo "!!  Incremental build did not pick up version.txt -> clean rebuild."
        "$PIO" run -e "$env" --target clean
        "$PIO" run -e "$env"
    fi

    local got
    got="$(embedded_version "$bin")"
    if [ "$got" != "$VERSION" ]; then
        echo "ERROR: firmware reports '${got}', expected '${VERSION}'. Aborting." >&2
        exit 1
    fi
    echo "==> Version verified: embedded=${got} == version.txt=${VERSION}"
}

# ------------------------------------------------------------
# Mode: "ota" → build the firmware and upload it to the server via SCP
# No argument → USB flash + serial monitor (as before)
# ------------------------------------------------------------

if [ "$1" = "ota" ]; then
    "$PROJECT_DIR/build.sh"
    "$PROJECT_DIR/OTA_update.sh" "$PROJECT_DIR/binary/fountain-${VERSION}.bin"
else
    build_versioned esp32s3

    echo "==> Flashing via USB..."
    $PIO run -e esp32s3 --target upload

    echo "==> Waiting for USB reconnect..."
    sleep 2
    echo "==> Starting serial monitor (Ctrl+C to exit)..."
    $PIO device monitor
fi
