#!/usr/bin/env python3
# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
"""build_device.py — generates the device-specific production artefacts
(serienfertigung.md §16): factory.bin + manufacturing_manifest.json.

    python3 tools/build_device.py --serial FNT-000001 [--version <x.y.z>]

Prerequisite: dist/<version>/release/ exists (tools/build_release.py).
Compiles NO firmware — the release image is identical for all devices
(stage 2). Status transition: allocated/provisioned -> provisioned.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import gen_factory_nvs

FW_ROOT = Path(__file__).resolve().parent.parent
DEVICES_JSON = FW_ROOT / "production" / "devices.json"
PENV_PY = Path.home() / ".platformio" / "penv" / "bin" / "python"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cert_cn(cert: Path) -> str:
    out = subprocess.run(["openssl", "x509", "-in", str(cert), "-noout", "-subject"],
                         capture_output=True, text=True, check=True).stdout
    for part in out.replace("subject=", "").split(","):
        k, _, v = part.strip().partition("=")
        if k.strip() == "CN":
            return v.strip()
    return ""


def nvs_sanity_keys(bin_path: Path) -> list:
    """Key NAMES (never values!) from factory.bin — via the IDF nvs_tool."""
    tools = sorted(Path.home().glob(
        ".platformio/packages/framework-espidf*/components/nvs_flash/nvs_partition_tool/nvs_tool.py"))
    if not tools:
        return []
    r = subprocess.run([str(PENV_PY), str(tools[-1]), "-d", "minimal", str(bin_path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return []
    # Format: " device:serial = <value>" — extract only the key names;
    # values (PEMs, bearer) must NEVER end up in the log.
    import re
    return re.findall(r"^\s*device:(\w+)\s*(?:\[\d+\])?\s*=",
                      r.stdout, flags=re.MULTILINE)


def update_status(serial: str, status: str, extra: dict | None = None):
    with open(DEVICES_JSON) as f:
        db = json.load(f)
    rec = next(d for d in db["devices"] if d["serial"] == serial)
    rec["manufacturing"]["status"] = status
    if extra:
        rec["manufacturing"].update(extra)
    tmp = DEVICES_JSON.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(db, f, indent=2, ensure_ascii=False)
        f.write("\n")
    tmp.replace(DEVICES_JSON)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True)
    ap.add_argument("--version", default=None)
    args = ap.parse_args()

    version = args.version or (FW_ROOT / "version.txt").read_text().strip()
    release = FW_ROOT / "dist" / version / "release"
    manifest_path = release / "build_manifest.json"
    if not manifest_path.exists():
        sys.exit(f"ERROR: {manifest_path} missing — run tools/build_release.py first")
    build_manifest = json.loads(manifest_path.read_text())

    db, rec = gen_factory_nvs.load_record(args.serial)
    status = rec["manufacturing"]["status"]
    if status not in ("allocated", "provisioned"):
        sys.exit(f"ERROR: {args.serial} has status {status!r} — "
                 f"expected allocated/provisioned")

    # Check secrets (gen_factory_nvs checks existence; here additionally the CN).
    sroot = FW_ROOT / db["defaults"]["secrets_root"]
    cert = sroot / rec["security"]["client_cert"]
    cn = cert_cn(cert)
    if cn != rec["device_id"]:
        sys.exit(f"ERROR: certificate CN {cn!r} != device_id {rec['device_id']!r}")

    devdir = FW_ROOT / "dist" / version / "devices" / args.serial
    factory_bin = devdir / "factory.bin"
    gen_factory_nvs.generate(args.serial, factory_bin)

    keys = nvs_sanity_keys(factory_bin)
    expected = {"serial", "device_id", "hw_rev", "server", "server_port", "bearer",
                "hmac_key", "hmac_kid", "batch", "prod_date", "client_cert", "client_key"}
    if keys and not expected.issubset(set(keys)):
        sys.exit(f"ERROR: factory.bin incomplete — missing keys: "
                 f"{sorted(expected - set(keys))}")
    print(f"factory.bin sanity: {len(keys) or 'nvs_tool n/a —'} keys "
          f"{'OK' if keys else '(size check only)'}")

    mm = {
        "serial": rec["serial"],
        "serial_u64": rec["serial_u64"],
        "device_id": rec["device_id"],
        "firmware": {
            "version": build_manifest["firmware_version"],
            "build": build_manifest["build_version"],
            "git": build_manifest["git_commit"],
        },
        "hardware": {"revision": rec["hw_revision"]},
        "server": rec["server"],
        "production": {"batch": rec["manufacturing"]["batch"]},
        "images": {
            "firmware_sha256": build_manifest["firmware"]["sha256"],
            "factory_sha256": sha256(factory_bin),
        },
    }
    (devdir / "manufacturing_manifest.json").write_text(json.dumps(mm, indent=2) + "\n")

    update_status(args.serial, "provisioned")
    print(f"{args.serial} provisioned -> {devdir}")


if __name__ == "__main__":
    main()
