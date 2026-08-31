#!/usr/bin/env python3
# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
"""register_server_devices.py — registers a production device in the
device registry of the fountainer_server (devices.json).

    python3 tools/register_server_devices.py --serial FNT-000001

Strictly additive: existing entries (in particular the operational device
esp32-a1b2c3d4e5f6) are NEVER modified; an already registered
device_id leads to an abort. The registry is only read at server start
-> restart the server afterwards (bash stop.sh && bash start.sh).
"""
import argparse
import json
import sys
from pathlib import Path

FW_ROOT = Path(__file__).resolve().parent.parent
DEVICES_JSON = FW_ROOT / "production" / "devices.json"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True, help="FNT label, e.g. FNT-000001")
    ap.add_argument("--server-repo", default="../fountainer_server")
    args = ap.parse_args()

    with open(DEVICES_JSON) as f:
        db = json.load(f)
    rec = next((d for d in db["devices"] if d["serial"] == args.serial), None)
    if rec is None:
        sys.exit(f"ERROR: {args.serial} not in {DEVICES_JSON}")

    secrets_root = FW_ROOT / db["defaults"]["secrets_root"]
    hmac_hex = (secrets_root / rec["security"]["hmac_key"]).read_text().strip()
    if len(bytes.fromhex(hmac_hex)) != 32:
        sys.exit("ERROR: hmac.key is not a 32-byte hex key")

    srv_json = (FW_ROOT / args.server_repo / "devices.json").resolve()
    with open(srv_json) as f:
        registry = json.load(f)

    device_id = rec["device_id"]
    if device_id in registry:
        sys.exit(f"ERROR: {device_id} is already registered in {srv_json} — "
                 f"entries are never overwritten")

    registry[device_id] = {
        "serial": f"{int(rec['serial_u64'], 16):016X}",
        "bearer_token": rec["security"]["bearer_token"],
        "auth_keys": {rec["security"]["hmac_key_id"]: hmac_hex},
    }

    tmp = srv_json.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(registry, f, indent=2)
        f.write("\n")
    tmp.replace(srv_json)

    print(f"registered: {device_id} (serial {registry[device_id]['serial']}) in {srv_json}")
    print("Restart needed: cd ../fountainer_server && bash stop.sh && bash start.sh")
    print("(The operational device goes offline for ~15 s and reconnects —")
    print(" before further steps, check in the admin web UI that it is online again.)")


if __name__ == "__main__":
    main()
