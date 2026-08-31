#!/bin/bash
# Fountainer: complete, signed firmware build.
#
#   ./build.sh          builds the OTA image (env esp32s3_ota, RSA-3072-signed)
#
# After a successful build the result is stored versioned under
#   binary/fountain-<version>.bin
# and is thereby ready for ./OTA_update.sh.
#
# Contains the version guard against the PlatformIO trap: version.txt is
# only embedded at CMake configure time; an incremental build can write a
# stale version into the image -> the server would offer the same OTA
# endlessly. The guard reads the ACTUALLY embedded version from the
# app descriptor and forces a clean rebuild on mismatch.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
ENV="esp32s3_ota"
BIN="$PROJECT_DIR/.pio/build/$ENV/firmware.bin"
OUT_DIR="$PROJECT_DIR/binary"

VERSION=$(tr -d '[:space:]' < "$PROJECT_DIR/version.txt")

# Embedded version from the app descriptor (char[32] @ offset 0x30).
embedded_version() {
    dd if="$1" bs=1 skip=48 count=32 2>/dev/null | tr -d '\0'
}

echo "==> Building firmware (env=$ENV, version.txt=v$VERSION)..."
"$PIO" run -e "$ENV" -d "$PROJECT_DIR"

if [ "$(embedded_version "$BIN")" != "$VERSION" ]; then
    echo "!!  Embedded version ($(embedded_version "$BIN")) != version.txt ($VERSION)"
    echo "!!  -> Clean rebuild (incremental build did not pick up version.txt)"
    "$PIO" run -e "$ENV" -d "$PROJECT_DIR" --target clean
    "$PIO" run -e "$ENV" -d "$PROJECT_DIR"
fi

GOT="$(embedded_version "$BIN")"
if [ "$GOT" != "$VERSION" ]; then
    echo "ERROR: firmware reports '$GOT', expected '$VERSION'. Aborting." >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
TARGET="$OUT_DIR/fountain-$VERSION.bin"
cp "$BIN" "$TARGET"

echo "==> OK: version $VERSION verified and stored:"
ls -la "$TARGET"
echo "==> Deployment: ./OTA_update.sh binary/fountain-$VERSION.bin"
