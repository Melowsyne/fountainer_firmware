# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
#
# PlatformIO pre-script (runs before CMake):
#   1. guarantees that src/network.json exists — the real file is git-ignored;
#      on a fresh checkout it is created from the committed network.json.example.
#   2. generates src/network_json_gen.c (git-ignored) with the file content as
#      a C string (g_network_json), consumed by network_config.c. This avoids
#      the flaky EMBED_TXTFILES/embed_txtfiles paths under PlatformIO+espidf.
#   3. generates src/certs_gen.c (git-ignored) with the TLS material from
#      src/certs/ (ca.crt.pem, client.crt.pem, client.key.pem — copied from the
#      testbed PKI ../CA). Missing files become empty strings -> the firmware
#      then falls back to plaintext ws:// (see task_com.c).
import shutil
from pathlib import Path

Import("env")  # noqa: F821  (PlatformIO SCons context)

root = Path(env["PROJECT_DIR"])  # noqa: F821


def c_escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def write_if_changed(path: Path, content: str) -> None:
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8")
        print(f"ensure_network_json: {path.name} regenerated")


# --- 1+2: network.json -> network_json_gen.c ---------------------------------
netdir = root / "src" / "network"
real = netdir / "network.json"
example = netdir / "network.json.example"
if not real.exists():
    shutil.copyfile(example, real)
    print(f"ensure_network_json: {real} created from {example.name}")

write_if_changed(netdir / "network_json_gen.c", (
    "/* AUTO-GENERATED from src/network.json by tools/ensure_network_json.py.\n"
    " * Do not edit, do not commit (git-ignored — contains provisioning data). */\n"
    f'const char g_network_json[] = "{c_escape(real.read_text(encoding="utf-8"))}";\n'
))

# --- 3: src/network/certs/ -> certs_gen.c --------------------------------------
certs = netdir / "certs"


def pem(name: str) -> str:
    p = certs / name
    return c_escape(p.read_text(encoding="utf-8")) if p.exists() else ""


ca, crt, key = pem("ca.crt.pem"), pem("client.crt.pem"), pem("client.key.pem")

# Series production: production images are identical for ALL devices — the
# device-individual identity (client certificate/key) comes from the
# factory NVS partition, NEVER from the binary. The shared CA stays in.
if env["PIOENV"] == "esp32s3_prod":  # noqa: F821
    if not ca:
        raise SystemExit("ensure_network_json: ERROR — ca.crt.pem missing, "
                         "production build without CA forbidden")
    crt = key = ""
    print("ensure_network_json: esp32s3_prod — client certificate/key are "
          "NOT embedded (identity from factory partition)")
elif not (ca and crt and key):
    print("ensure_network_json: WARNING — TLS material incomplete, "
          "firmware falls back to plaintext ws://")

write_if_changed(netdir / "certs_gen.c", (
    "/* AUTO-GENERATED from src/certs/ by tools/ensure_network_json.py.\n"
    " * Do not edit, do not commit (git-ignored — contains the device key). */\n"
    f'const char g_tls_ca_pem[] = "{ca}";\n'
    f'const char g_tls_client_cert_pem[] = "{crt}";\n'
    f'const char g_tls_client_key_pem[] = "{key}";\n'
))
