#!/bin/bash
# Fountainer: deploy the OTA image to the server.
#
#   ./OTA_update.sh binary/fountain-4.30.0.bin
#
# Copies the given firmware image into the FIRMWARE_UPDATES folder of the
# sibling project fountainer_server (relative path). The server scans
# the folder and offers the image at the device's next ota_check;
# a reboot command (web UI) triggers the cycle immediately.
#
# Safety nets:
#   - checks the ESP32 app image magic (0xE9)
#   - reads the EMBEDDED version from the app descriptor and names the
#     target file after it (fountain-<version>.bin) — file name and reported
#     version can thus never diverge (endless-OTA trap)
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$PROJECT_DIR/../fountainer_server/FIRMWARE_UPDATES"

if [ $# -ne 1 ]; then
    echo "Usage: $0 <firmware.bin>   (e.g. binary/fountain-4.30.0.bin)" >&2
    exit 1
fi

SRC="$1"
[ -f "$SRC" ] || { echo "ERROR: file '$SRC' not found." >&2; exit 1; }
[ -d "$DEST_DIR" ] || { echo "ERROR: target folder '$DEST_DIR' missing (fountainer_server checked out?)." >&2; exit 1; }

# An ESP32 app image starts with the magic byte 0xE9.
MAGIC=$(dd if="$SRC" bs=1 count=1 2>/dev/null | od -An -tx1 | tr -d ' ')
[ "$MAGIC" = "e9" ] || { echo "ERROR: '$SRC' is not an ESP32 app image (magic 0x$MAGIC != 0xe9)." >&2; exit 1; }

# Embedded version from the app descriptor (char[32] @ offset 0x30).
VERSION=$(dd if="$SRC" bs=1 skip=48 count=32 2>/dev/null | tr -d '\0')
[ -n "$VERSION" ] || { echo "ERROR: no embedded version found in the image." >&2; exit 1; }

BASENAME=$(basename "$SRC")
TARGET="$DEST_DIR/fountain-$VERSION.bin"
if [ "$BASENAME" != "fountain-$VERSION.bin" ]; then
    echo "NOTE: file name '$BASENAME' != embedded version -> target is named fountain-$VERSION.bin"
fi

cp "$SRC" "$TARGET"
echo "==> Deployed: $TARGET (embedded version: $VERSION)"
echo "==> The server offers the update at the device's next connect"
echo "    (trigger immediately: reboot command in the web UI, port 8010)."
