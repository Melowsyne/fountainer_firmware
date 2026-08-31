#!/usr/bin/env python3
# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
"""verify_device.py — production verification after flashing
(serienfertigung.md §22): reads the boot log over USB serial and checks the
FACTORY-ID line against the production record + release manifest.

    python3 tools/verify_device.py --serial FNT-000001 --port /dev/ttyACM0 \
        [--timeout 120] [--expect-connect]

Checks: serial/device_id/HW/FW/build version, identity source MUST be
"factory", no boot loop (>3 resets => FAIL), optionally the
server session ("protocol ready"). Result: status passed/failed,
verify_report.json in the dist device directory, line in production.log.
"""
import argparse
import datetime
import json
import re
import sys
import time
from pathlib import Path

import serial  # pyserial (available in the PlatformIO penv)

FW_ROOT = Path(__file__).resolve().parent.parent
DEVICES_JSON = FW_ROOT / "production" / "devices.json"
PROD_LOG = FW_ROOT / "production" / "production.log"

FACTORY_RE = re.compile(
    r"FACTORY-ID serial=([0-9A-F]{16}) device_id=(\S+) hw=(\S+) "
    r"fw=(\S+) build=(\d+) src=(\w+)")
READY_MARKER = "protocol ready (negotiated, running)"


def reset_device(port: serial.Serial):
    """Classic esptool reset sequence (USB-Serial-JTAG: EN via RTS)."""
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False


def update_status(serial_label: str, status: str):
    with open(DEVICES_JSON) as f:
        db = json.load(f)
    rec = next(d for d in db["devices"] if d["serial"] == serial_label)
    rec["manufacturing"]["status"] = status
    tmp = DEVICES_JSON.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(db, f, indent=2, ensure_ascii=False)
        f.write("\n")
    tmp.replace(DEVICES_JSON)
    return rec


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--version", default=None)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--expect-connect", action="store_true",
                    help="additionally wait for the server session")
    args = ap.parse_args()

    version = args.version or (FW_ROOT / "version.txt").read_text().strip()
    devdir = FW_ROOT / "dist" / version / "devices" / args.serial
    mm = json.loads((devdir / "manufacturing_manifest.json").read_text())

    with open(DEVICES_JSON) as f:
        db = json.load(f)
    rec = next((d for d in db["devices"] if d["serial"] == args.serial), None)
    if rec is None:
        sys.exit(f"ERROR: {args.serial} not in {DEVICES_JSON}")

    expected = {
        "serial": f"{int(rec['serial_u64'], 16):016X}",
        "device_id": rec["device_id"],
        "hw": rec["hw_revision"],
        "fw": mm["firmware"]["version"],
        "build": str(mm["firmware"]["build"]),
        "src": "factory",
    }

    checks = {}
    resets = 0
    connected = False
    lines = []
    deadline = time.monotonic() + args.timeout

    with serial.Serial(args.port, 115200, timeout=1) as port:
        reset_device(port)
        while time.monotonic() < deadline:
            raw = port.readline()
            if not raw:
                continue
            line = raw.decode(errors="replace").rstrip()
            lines.append(line)
            if line.startswith("rst:"):
                resets += 1
                if resets > 3:
                    checks["boot_loop"] = f"FAIL ({resets} resets)"
                    break
            m = FACTORY_RE.search(line)
            if m:
                got = dict(zip(("serial", "device_id", "hw", "fw", "build", "src"),
                               m.groups()))
                for k, want in expected.items():
                    checks[k] = "OK" if got[k] == want else f"FAIL (got {got[k]!r}, want {want!r})"
                if not args.expect_connect:
                    break
            if args.expect_connect and READY_MARKER in line:
                connected = True
                checks["session"] = "OK"
                break
        else:
            if "serial" not in checks:
                checks["factory_id"] = "FAIL (FACTORY-ID line not seen)"
            if args.expect_connect and not connected:
                checks["session"] = "FAIL (no server session within the time window)"

    checks.setdefault("boot_loop", "OK" if resets <= 3 else f"FAIL ({resets})")
    if args.expect_connect:
        checks.setdefault("session", "OK" if connected else "FAIL")
    passed = all(v.startswith("OK") for v in checks.values()) and "serial" in checks

    for k, v in checks.items():
        print(f"  {k:12s} {v}")
    verdict = "PASS" if passed else "FAIL"
    print(f"\n{args.serial}: {verdict}")

    update_status(args.serial, "passed" if passed else "failed")
    report = {
        "serial": args.serial, "verdict": verdict, "checks": checks,
        "firmware": mm["firmware"],
        "timestamp": datetime.datetime.now().isoformat(timespec="seconds"),
        "log_tail": lines[-40:],
    }
    (devdir / "verify_report.json").write_text(json.dumps(report, indent=2) + "\n")
    with open(PROD_LOG, "a") as f:
        f.write(f"{report['timestamp']} {args.serial} {verdict} "
                f"fw={mm['firmware']['version']} build={mm['firmware']['build']} "
                f"checks={sum(v.startswith('OK') for v in checks.values())}/{len(checks)}\n")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
