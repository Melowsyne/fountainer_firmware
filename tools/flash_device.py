#!/usr/bin/env python3
# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
"""flash_device.py — production flashing of a device (serienfertigung.md §21).

    python3 tools/flash_device.py --serial FNT-000001 --port /dev/ttyACM0 \
        [--version <x.y.z>] [--erase-all]

Writes bootloader, partition table, otadata, firmware and the
device-specific factory.bin from dist/ (never from the build directory).
--port is mandatory (no auto-detection — protection against mix-ups: only the
production/test device on the given port may be flashed, never a
device in the field). Status transition: provisioned/flashed/failed -> flashed.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

FW_ROOT = Path(__file__).resolve().parent.parent
DEVICES_JSON = FW_ROOT / "production" / "devices.json"
PENV_PY = Path.home() / ".platformio" / "penv" / "bin" / "python"
ESPTOOL = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"

# Offsets: bootloader/table/otadata as in the build's flasher_args.json;
# factory @0xF40000 = partitions.csv row "factory".
FLASH_MAP = [
    ("0x0", "bootloader.bin"),
    ("0x8000", "partitions.bin"),
    ("0xf000", "ota_data_initial.bin"),
    ("0x20000", "firmware.bin"),
]
FACTORY_OFFSET = "0xf40000"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--version", default=None)
    ap.add_argument("--erase-all", action="store_true",
                    help="Erase the entire chip first (factory-new devices)")
    args = ap.parse_args()

    version = args.version or (FW_ROOT / "version.txt").read_text().strip()
    release = FW_ROOT / "dist" / version / "release"
    devdir = FW_ROOT / "dist" / version / "devices" / args.serial
    factory_bin = devdir / "factory.bin"

    with open(DEVICES_JSON) as f:
        db = json.load(f)
    rec = next((d for d in db["devices"] if d["serial"] == args.serial), None)
    if rec is None:
        sys.exit(f"ERROR: {args.serial} not in {DEVICES_JSON}")
    status = rec["manufacturing"]["status"]
    if status not in ("provisioned", "flashed", "failed"):
        sys.exit(f"ERROR: status {status!r} — expected provisioned/flashed/failed")

    build_manifest = json.loads((release / "build_manifest.json").read_text())
    mm = json.loads((devdir / "manufacturing_manifest.json").read_text())
    # Check artefacts against the manifests — only verified data gets flashed.
    for name, want in build_manifest["artifacts"].items():
        got = sha256(release / name)
        if got != want:
            sys.exit(f"ERROR: SHA256 mismatch {name}: {got} != {want}")
    if sha256(factory_bin) != mm["images"]["factory_sha256"]:
        sys.exit("ERROR: SHA256 mismatch factory.bin")
    if mm["firmware"]["build"] != build_manifest["build_version"]:
        sys.exit("ERROR: manufacturing_manifest belongs to a different release build")

    fs = build_manifest.get("flash_settings", {})
    # default_reset: esptool handles entering the bootloader via
    # USB-Serial-JTAG itself (verified on the testbed); if the sequence hangs,
    # manually holding BOOT + RESET helps (then no_reset does not matter).
    cmd = [str(PENV_PY), str(ESPTOOL), "--chip", "esp32s3",
           "--port", args.port, "--baud", "460800",
           "--before", "default_reset", "--after", "hard_reset"]
    if args.erase_all:
        print("Erasing entire flash ...")
        subprocess.run(cmd + ["erase_flash"], check=True)
    write = cmd + ["write_flash",
                   "--flash_mode", fs.get("flash_mode", "dio"),
                   "--flash_freq", fs.get("flash_freq", "80m"),
                   "--flash_size", fs.get("flash_size", "16MB")]
    for off, name in FLASH_MAP:
        write += [off, str(release / name)]
    write += [FACTORY_OFFSET, str(factory_bin)]
    print("+", " ".join(write[2:]))
    subprocess.run(write, check=True)

    rec["manufacturing"]["status"] = "flashed"
    rec["manufacturing"]["firmware_version"] = build_manifest["firmware_version"]
    rec["manufacturing"]["build_version"] = build_manifest["build_version"]
    tmp = DEVICES_JSON.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(db, f, indent=2, ensure_ascii=False)
        f.write("\n")
    tmp.replace(DEVICES_JSON)
    print(f"{args.serial} flashed (fw {build_manifest['firmware_version']}, "
          f"build {build_manifest['build_version']}) — continue with verify_device.py")


if __name__ == "__main__":
    main()
