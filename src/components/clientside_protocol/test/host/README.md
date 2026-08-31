# Host tests (clientside_protocol)

The interop-critical modules (`fp_auth`, `fp_envelope`, `fp_session`, `fountain_msgs`)
depend only on **cJSON** + **mbedTLS** and can therefore be tested on an ordinary
Linux host — without ESP-IDF. `fp_ws`/`fp_task` are ESP-IDF-specific and are not
built here.

## Recommended: test runner

```bash
cd clientside_protocol
./test/host/run_host_tests.sh
```

The runner builds cJSON **and** mbedTLS from **one** source (the PlatformIO
ESP-IDF toolchain) and therefore needs no system dev packages. Both tests
end with `GOLDEN VECTOR : OK` and `SESSION TEST OK` respectively.

> **Why not simply link against the system library?** A build with the
> Espressif mbedTLS **headers** against the system `libmbedcrypto.so`
> compiles and links, but mixes two ABIs: the `mbedtls_sha256_context`
> layout differs, SHA256 silently produces a **wrong** hash, and the golden
> test falsely reports `MISMATCH`. The runner avoids this by taking headers
> and code from the same tree.

With the system dev packages installed (`libcjson-dev libmbedtls-dev
build-essential`) this also works:

```bash
USE_SYSTEM=1 ./test/host/run_host_tests.sh
```

## Manual build (system dev packages only, ABI-consistent)

```bash
cd clientside_protocol

# 1) golden auth vector (must reproduce AUTH-CONTRACT.md)
gcc -Wall -Iinclude -I/usr/include/cjson \
    test/host/test_auth_golden.c src/fp_auth.c \
    -lcjson -lmbedcrypto -o /tmp/test_auth && /tmp/test_auth

# 2) full session path (hello -> ota_check -> ota_none -> command -> replay)
gcc -Wall -Iinclude -I/usr/include/cjson \
    test/host/test_session.c \
    src/fp_session.c src/fp_envelope.c src/fp_auth.c src/fountain_msgs.c \
    -lcjson -lmbedcrypto -o /tmp/test_session && /tmp/test_session
```

Both tests end with exit 0 and print `GOLDEN VECTOR : OK` and `SESSION TEST OK`.

Via Docker (without a host installation):

```bash
docker run --rm -v "$PWD/../../..":/work --entrypoint bash debian:bookworm-slim -c '
  apt-get update -qq && apt-get install -y -qq build-essential libcjson-dev libmbedtls-dev
  cd /work/esp32-s3_fountain_framework_v1/clientside_protocol
  gcc -Iinclude -I/usr/include/cjson test/host/test_session.c \
      src/fp_session.c src/fp_envelope.c src/fp_auth.c src/fountain_msgs.c \
      -lcjson -lmbedcrypto -o /tmp/t && /tmp/t'
```
