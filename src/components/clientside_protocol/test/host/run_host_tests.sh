#!/usr/bin/env bash
# Host test runner for clientside_protocol (fp_auth / fp_session / ...).
#
# WHY this script: the tests need cJSON + mbedTLS. A naive build against the
# system runtime lib (libmbedcrypto.so.7) with the Espressif mbedTLS HEADERS
# mixes two ABIs — it compiles and LINKS, but produces a silently WRONG hash
# (the SHA256 context layout differs) and the golden test then falsely
# reports MISMATCH. This script builds cJSON AND mbedTLS from ONE source (the
# PlatformIO ESP-IDF toolchain), so that headers and code match each other.
# Alternatively (USE_SYSTEM=1) build against the system dev packages.
#
# Usage:  ./run_host_tests.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CP="$(cd "$HERE/../.." && pwd)"          # clientside_protocol/
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Work from here so that the relative source/include paths
# (include/, src/…) are correct regardless of the caller's CWD.
cd "$CP"

INC=(-Iinclude)
CJSON_SRC=()
MB_OBJS=()
LIBS=()

if [ "${USE_SYSTEM:-0}" = "1" ]; then
    echo "== Mode: system dev packages (libcjson-dev / libmbedtls-dev) =="
    INC+=(-I/usr/include/cjson)
    LIBS=(-lcjson -lmbedcrypto)
else
    PIO_PKGS="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
    # Find cJSON in an installed IDF. Caution: since IDF 6 there is no
    # components/json any more — the project is pinned to IDF 5.5.x
    # (platformio.ini: espressif32@6.13.0 -> framework-espidf@3.5xxxx),
    # hence search all framework-espidf* directories.
    CJDIR=""
    for d in "$PIO_PKGS"/framework-espidf*/components/json/cJSON; do
        [ -f "$d/cJSON.c" ] && CJDIR="$d" && break
    done
    MBROOT="$(find "$PIO_PKGS" -type d -path '*mbedtls/repo' 2>/dev/null | head -1)"
    [ -n "$CJDIR" ] || { echo "ERROR: cJSON source not found in any framework-espidf* (IDF 5.5.x installed?)" >&2; exit 1; }
    [ -n "$MBROOT" ]        || { echo "ERROR: mbedTLS source not found in the toolchain." >&2; exit 1; }
    echo "== Mode: toolchain sources =="
    echo "   cJSON  : $CJDIR"
    echo "   mbedTLS: $MBROOT"

    # Minimal config: only HMAC-SHA256 + Base64, WITHOUT PSA crypto.
    CFG="$WORK/mini_mbedtls_config.h"
    cat > "$CFG" <<'EOF'
#ifndef MBEDTLS_MINI_CONFIG_H
#define MBEDTLS_MINI_CONFIG_H
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_ERROR_C
#endif
EOF
    DCFG="-DMBEDTLS_CONFIG_FILE=\"$CFG\""    # stays ONE argv element (no eval)
    # Exactly the translation units for HMAC-SHA256 + Base64.
    for u in md sha256 base64 platform_util constant_time; do
        gcc "$DCFG" -I"$MBROOT/include" -c "$MBROOT/library/$u.c" -o "$WORK/$u.o"
        MB_OBJS+=("$WORK/$u.o")
    done
    INC+=(-I"$CJDIR" -I"$MBROOT/include" "$DCFG")
    CJSON_SRC=("$CJDIR/cJSON.c")
fi

FAIL=0

echo; echo "== 1) Golden Auth Vector (AUTH-CONTRACT.md) =="
if gcc -Wall "${INC[@]}" "$HERE/test_auth_golden.c" src/fp_auth.c \
       "${CJSON_SRC[@]}" "${MB_OBJS[@]}" "${LIBS[@]}" -o "$WORK/test_auth"; then
    ( cd "$CP" && "$WORK/test_auth" ) || FAIL=1
else
    FAIL=1
fi

echo; echo "== 2) Session path (hello -> ota_check -> ota_none -> command -> replay) =="
if gcc -Wall "${INC[@]}" "$HERE/test_session.c" \
       src/fp_session.c src/fp_envelope.c src/fp_auth.c src/fountain_msgs.c \
       "${CJSON_SRC[@]}" "${MB_OBJS[@]}" "${LIBS[@]}" -o "$WORK/test_session"; then
    ( cd "$CP" && "$WORK/test_session" ) || FAIL=1
else
    FAIL=1
fi

echo
if [ "$FAIL" = "0" ]; then
    echo "== ALL HOST TESTS PASSED =="
else
    echo "== HOST TESTS FAILED ==" >&2
fi
exit $FAIL
