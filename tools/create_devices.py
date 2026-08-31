#!/usr/bin/env python3
# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
"""create_devices.py — creates new device records for series production.

    python3 tools/create_devices.py --batch 2026-08-A \
        --mac 44:1b:f6:ce:f7:84 [--mac ...] \
        --hw-revision HW1.0 --server 192.168.1.12 [--port 8443]

Per device: next free FNT number (never reused), device_id in the
existing-device scheme esp32-<mac> (WiFi STA MAC of the specific device,
mandatory --mac, one per device), secrets directory under
<secrets_root>/FNT-xxxxxx/ (HMAC key + combined clientAuth+serverAuth
certificate via ../DO_NOT_COMMIT/CA/issue_device_cert.sh v3_device — the device
needs the server role for local WSS maintenance access), bearer token, entry
in production/devices.json (status "allocated").

Security rules (design spec serienfertigung.md §13/§27): uniqueness of
serial, serial_u64, record_id, device_id, cert path and (device_id, kid);
existing records/secrets are NEVER overwritten.
"""
import argparse
import datetime
import json
import re
import secrets
import subprocess
import sys
from pathlib import Path

FW_ROOT = Path(__file__).resolve().parent.parent
DEVICES_JSON = FW_ROOT / "production" / "devices.json"
# Serial scheme: 0x00464E5400000000 + N  ("\0FNT" + running number) —
# human-readable in a hex dump and collision-free with the MAC-based legacy
# serial of the existing device (0x000001C0C01FA82A).
SERIAL_BASE = 0x00464E5400000000

SCHEMA_VERSION = 1


def normalize_mac(raw: str) -> str:
    """Normalize a MAC to 12 lowercase hex digits (44:1b:f6:ce:f7:84,
    44-1B-…, 441bf6cef784 are equivalent)."""
    mac = re.sub(r"[:\-.]", "", raw).lower()
    if not re.fullmatch(r"[0-9a-f]{12}", mac):
        sys.exit(f"ERROR: invalid MAC address: {raw!r}")
    return mac


def load_db():
    if DEVICES_JSON.exists():
        with open(DEVICES_JSON) as f:
            db = json.load(f)
        if db.get("schema_version") != SCHEMA_VERSION:
            sys.exit(f"ERROR: unknown schema_version in {DEVICES_JSON}")
        return db
    return {
        "schema_version": SCHEMA_VERSION,
        "defaults": {
            "server_host": "192.168.1.12",
            "server_port": 8443,
            "server_path": "/ws",
            "hw_revision": "HW1.0",
            "secrets_root": "../DO_NOT_COMMIT/production_secrets",
        },
        "devices": [],
    }


def save_db_atomic(db):
    DEVICES_JSON.parent.mkdir(parents=True, exist_ok=True)
    tmp = DEVICES_JSON.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(db, f, indent=2, ensure_ascii=False)
        f.write("\n")
    tmp.replace(DEVICES_JSON)


def check_unique(db):
    """Aborts on any violation — never continue silently."""
    seen = {"serial": set(), "serial_u64": set(), "record_id": set(),
            "device_id": set(), "cert": set(), "kid_pair": set()}
    for d in db["devices"]:
        for field, key in (("serial", "serial"), ("serial_u64", "serial_u64"),
                           ("record_id", "record_id"), ("device_id", "device_id")):
            v = d[key]
            if v in seen[field]:
                sys.exit(f"ERROR: duplicate {field}: {v}")
            seen[field].add(v)
        cert = d["security"]["client_cert"]
        if cert in seen["cert"]:
            sys.exit(f"ERROR: duplicate certificate path: {cert}")
        seen["cert"].add(cert)
        pair = (d["device_id"], d["security"]["hmac_key_id"])
        if pair in seen["kid_pair"]:
            sys.exit(f"ERROR: duplicate (device_id, kid): {pair}")
        seen["kid_pair"].add(pair)


def next_fnt_number(db):
    n = 0
    for d in db["devices"]:
        m = re.fullmatch(r"FNT-(\d{6})", d["serial"])
        if m:
            n = max(n, int(m.group(1)))
    return n + 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--count", type=int, default=None,
                    help="Number of devices (default: number of --mac arguments)")
    ap.add_argument("--mac", action="append", required=True, metavar="MAC",
                    help="WiFi STA MAC of the device (one per device) — becomes "
                         "the device_id esp32-<mac> (display scheme of the "
                         "existing device)")
    ap.add_argument("--batch", required=True)
    ap.add_argument("--hw-revision", default=None)
    ap.add_argument("--server", default=None)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--secrets-root", default=None,
                    help="Overrides defaults.secrets_root (relative to FW root)")
    ap.add_argument("--ca-dir", default="../DO_NOT_COMMIT/CA",
                    help="Testbed CA with issue_device_cert.sh (relative to FW root)")
    args = ap.parse_args()

    macs = [normalize_mac(m) for m in args.mac]
    if len(set(macs)) != len(macs):
        sys.exit("ERROR: duplicate MAC address in --mac")
    if args.count is not None and args.count != len(macs):
        sys.exit(f"ERROR: --count {args.count} does not match "
                 f"{len(macs)} --mac argument(s)")

    db = load_db()
    check_unique(db)
    defaults = db["defaults"]
    hw = args.hw_revision or defaults["hw_revision"]
    server = args.server or defaults["server_host"]
    port = args.port or defaults["server_port"]
    secrets_root = FW_ROOT / (args.secrets_root or defaults["secrets_root"])
    ca_dir = FW_ROOT / args.ca_dir
    issue = ca_dir / "issue_device_cert.sh"
    if not issue.exists():
        sys.exit(f"ERROR: {issue} missing")

    created = []
    n = next_fnt_number(db)
    for i, mac in enumerate(macs):
        num = n + i
        fnt = f"FNT-{num:06d}"
        serial_u64 = SERIAL_BASE + num
        device_id = f"esp32-{mac}"

        # Collisions against existing records (incl. serial_u64 as hex string).
        for d in db["devices"]:
            if d["serial"] == fnt or d["device_id"] == device_id \
               or int(d["serial_u64"], 16) == serial_u64:
                sys.exit(f"ERROR: {fnt}/{device_id} collides with record_id {d['record_id']}")

        sdir = secrets_root / fnt
        if sdir.exists():
            sys.exit(f"ERROR: secrets directory {sdir} already exists — "
                     f"serial numbers are never reused")
        sdir.mkdir(parents=True)
        sdir.chmod(0o700)

        hmac_key = secrets.token_bytes(32)
        (sdir / "hmac.key").write_text(hmac_key.hex() + "\n")
        (sdir / "hmac.key").chmod(0o600)

        # Combined cert clientAuth+serverAuth (v3_device): the device is cloud
        # client AND local WSS server — a pure v3_client cert would repeat the
        # EKU error of the existing device (fix from 16.08.).
        subprocess.run(["bash", str(issue), device_id, str(sdir), "v3_device"],
                       check=True)

        record = {
            "record_id": max((d["record_id"] for d in db["devices"]), default=-1) + 1,
            "serial": fnt,
            "serial_u64": f"0x{serial_u64:016X}",
            "device_id": device_id,
            "hw_revision": hw,
            "server": {"host": server, "port": port},
            "security": {
                "client_cert": f"{fnt}/client.crt",
                "client_key": f"{fnt}/client.key",
                "hmac_key": f"{fnt}/hmac.key",
                "hmac_key_id": "1",
                "bearer_token": secrets.token_urlsafe(32),
            },
            "manufacturing": {
                "batch": args.batch,
                "status": "allocated",
                "created": datetime.date.today().isoformat(),
            },
        }
        db["devices"].append(record)
        created.append((fnt, device_id))

    check_unique(db)
    save_db_atomic(db)
    for fnt, device_id in created:
        print(f"created: {fnt} ({device_id})")
    print(f"{len(created)} device(s) in {DEVICES_JSON}")


if __name__ == "__main__":
    main()
