#!/usr/bin/env python3
# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
"""gen_factory_nvs.py — generates the factory NVS partition (factory.bin) for
a device from production/devices.json (serienfertigung.md §18).

    python3 tools/gen_factory_nvs.py --serial FNT-000001 --out <dir>/factory.bin

The key names/types correspond exactly to the reads in
src/components/factory_config/factory_config.c (namespace "device").
The temporary CSV contains the private key — it is created in a
0700 temp directory and deleted in `finally:` (§27.9).
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW_ROOT = Path(__file__).resolve().parent.parent
DEVICES_JSON = FW_ROOT / "production" / "devices.json"
PENV_PY = Path.home() / ".platformio" / "penv" / "bin" / "python"


def load_record(serial: str):
    with open(DEVICES_JSON) as f:
        db = json.load(f)
    rec = next((d for d in db["devices"] if d["serial"] == serial), None)
    if rec is None:
        sys.exit(f"ERROR: {serial} not in {DEVICES_JSON}")
    return db, rec


def generate(serial: str, out: Path, size: int = 0x10000) -> Path:
    db, rec = load_record(serial)
    sroot = FW_ROOT / db["defaults"]["secrets_root"]
    cert = sroot / rec["security"]["client_cert"]
    key = sroot / rec["security"]["client_key"]
    hmac_hex = (sroot / rec["security"]["hmac_key"]).read_text().strip()
    for p in (cert, key):
        if not p.is_file() or p.stat().st_size == 0:
            sys.exit(f"ERROR: secret missing/empty: {p}")
    if len(bytes.fromhex(hmac_hex)) != 32:
        sys.exit("ERROR: hmac.key is not a 32-byte hex key")
    for p, cap in ((cert, 3072), (key, 3072)):
        if p.stat().st_size >= cap:
            sys.exit(f"ERROR: {p} >= {cap} B (FACTORY_PEM_MAX in factory_config.h)")

    rows = [
        "key,type,encoding,value",
        "device,namespace,,",
        f"serial,data,u64,{int(rec['serial_u64'], 16)}",
        f"device_id,data,string,{rec['device_id']}",
        f"hw_rev,data,string,{rec['hw_revision']}",
        f"server,data,string,{rec['server']['host']}",
        f"server_port,data,u16,{rec['server']['port']}",
        f"bearer,data,string,{rec['security']['bearer_token']}",
        f"hmac_key,data,hex2bin,{hmac_hex}",
        f"hmac_kid,data,string,{rec['security']['hmac_key_id']}",
        f"batch,data,string,{rec['manufacturing']['batch']}",
        f"prod_date,data,string,{rec['manufacturing'].get('created', '')}",
        f"client_cert,file,string,{cert}",
        f"client_key,file,string,{key}",
    ]

    tmpdir = Path(tempfile.mkdtemp(prefix="factory_nvs_"))  # mkdtemp = mode 0700
    try:
        csv = tmpdir / f"{serial}.factory.csv"
        csv.write_text("\n".join(rows) + "\n")
        out.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [str(PENV_PY), "-m", "esp_idf_nvs_partition_gen", "generate",
             str(csv), str(out), f"0x{size:X}"],
            check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        sys.exit(f"ERROR: nvs_partition_gen: {e.stderr}")
    finally:
        shutil.rmtree(tmpdir)  # the CSV contains the private key (§27.9)

    if out.stat().st_size != size:
        sys.exit(f"ERROR: {out} has {out.stat().st_size} B instead of {size}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x10000)
    args = ap.parse_args()
    out = generate(args.serial, args.out, args.size)
    print(f"factory.bin created: {out} ({args.size} B)")


if __name__ == "__main__":
    main()
