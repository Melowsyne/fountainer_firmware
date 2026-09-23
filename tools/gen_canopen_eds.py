#!/usr/bin/env python3
# Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
"""gen_canopen_eds.py — Electronic Data Sheet (CiA 306) for the CANopen slave.

Generates, from the datapoint catalog src/components/datapoints/dp_list.def
and version.txt, the object dictionary description the master needs:

    DOKU/canopen/fountainer.eds      EDS (python-canopen, CANopen tools)
    DOKU/canopen/fountainer_od.json  sidecar: name/index/type/access + UI
                                     annotations (unit, decimals, map)

and mirrors both into ../fountainer_can_master_linux/eds/ when that checkout
exists (the master ships its own copy).

Index layout (must match src/device/canopen_task.c):
    0x2000 + i   datapoint i in dp_list.def order, sub 0 = value
    0x2FFF       number of datapoints (U16)
    0x3000 + i   descriptor record: sub1 name (string), sub2 type (U8,
                 dp_type_t), sub3 access (U8: 0 RO, 1 RW, 2 WO)

Usage:
    standalone:  python3 tools/gen_canopen_eds.py [--out DIR] [--no-mirror]
    PlatformIO:  extra_scripts = pre:tools/gen_canopen_eds.py
"""
import json
import re
import sys
from datetime import datetime
from pathlib import Path

try:
    ROOT = Path(__file__).resolve().parent.parent
    STANDALONE = True
except NameError:                        # PlatformIO SCons exec() context
    Import("env")  # noqa: F821
    ROOT = Path(env["PROJECT_DIR"])  # noqa: F821
    STANDALONE = False

DEF_FILE = ROOT / "src/components/datapoints/dp_list.def"
VERSION_FILE = ROOT / "version.txt"
OUT_DIR = ROOT / "DOKU/canopen"
MIRROR_DIR = ROOT.parent / "fountainer_can_master_linux/eds"

# Same parser as tools/lint_datapoints.py (kept in sync by hand).
DP_RE = re.compile(
    r"^\s*DP\(\s*([A-Za-z0-9_]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,"
    r"\s*(\d+)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^)\s]+)\s*\)"
    r"(?:.*?/\*@\s*(.*?)\s*@\*/)?")

# dp_type_t order (datapoints.h) -> (CiA 306 data type, byte size)
DP_TYPES = {
    "BOOL": (0, 0x0001, 1), "U8": (1, 0x0005, 1), "U16": (2, 0x0006, 2),
    "U32": (3, 0x0007, 4), "U64": (4, 0x001B, 8), "I8": (5, 0x0002, 1),
    "I16": (6, 0x0003, 2), "I32": (7, 0x0004, 4), "F32": (8, 0x0008, 4),
    "ENUM": (9, 0x0005, 1), "STR": (10, 0x0009, 0),
}
ACCESS_CODE = {"RO": 0, "RW": 1, "WO": 2}
SECRET = {"Network_Password", "Backup_Password"}   # write-only over CAN
PRODUCT_CODE = 0x464E5400


def parse_catalog():
    rows = []
    for line in DEF_FILE.read_text(encoding="utf-8").splitlines():
        m = DP_RE.match(line)
        if not m:
            continue
        name, typ, acc, per, nvs_id, default, mn, mx, db, ann = m.groups()
        meta = {}
        for tok in (ann or "").split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                meta[k] = v
        rows.append({
            "name": name, "type": typ, "access": acc, "persist": per,
            "default": default.strip(), "min": mn.strip(), "max": mx.strip(),
            "unit": meta.get("unit"), "dec": meta.get("dec"),
            "map": meta.get("map"), "fmt": meta.get("fmt"),
        })
    return rows


def sw_revision(version):
    parts = [int(p) for p in version.split(".")[:3]] + [0, 0, 0]
    return ((parts[0] & 0xFF) << 16) | ((parts[1] & 0xFF) << 8) | (parts[2] & 0xFF)


def num_default(row):
    """EDS DefaultValue for a datapoint (NVS default, else 0/empty)."""
    if row["type"] == "STR":
        return ""
    if row["persist"] != "NVS":
        return "0"
    tok = row["default"].rstrip("fF")
    try:
        return repr(float(tok)) if row["type"] == "F32" else str(int(float(tok)))
    except ValueError:
        return "0"


def var(index, name, dtype, access, default="0", pdo=0, sub=None):
    sec = f"[{index:04X}]" if sub is None else f"[{index:04X}sub{sub:X}]"
    return "\n".join([
        sec,
        f"ParameterName={name}",
        "ObjectType=0x7",
        f"DataType=0x{dtype:04X}",
        f"AccessType={access}",
        f"DefaultValue={default}",
        f"PDOMapping={pdo}",
        "",
    ])


def record(index, name, subs, otype=0x9):
    out = [f"[{index:04X}]", f"ParameterName={name}", f"ObjectType=0x{otype:X}",
           f"SubNumber={len(subs)}", ""]
    for sub, (sname, dtype, access, default, pdo) in enumerate(subs):
        out.append(var(index, sname, dtype, access, default, pdo, sub=sub))
    return "\n".join(out)


def build_eds(rows, version):
    now = datetime.now()
    rev = sw_revision(version)
    n = len(rows)
    out = []
    out.append("\n".join([
        "[FileInfo]",
        "FileName=fountainer.eds",
        "FileVersion=1",
        "FileRevision=0",
        "EDSVersion=4.0",
        f"Description=Fountainer well-pump controller, CANopen slave (firmware {version})",
        f"CreationTime={now.strftime('%I:%M%p')}",
        f"CreationDate={now.strftime('%m-%d-%Y')}",
        "CreatedBy=tools/gen_canopen_eds.py",
        f"ModificationTime={now.strftime('%I:%M%p')}",
        f"ModificationDate={now.strftime('%m-%d-%Y')}",
        "ModifiedBy=tools/gen_canopen_eds.py",
        "",
        "[DeviceInfo]",
        "VendorName=Melowsyne Unipessoal Lda",
        "VendorNumber=0",
        "ProductName=Fountainer",
        f"ProductNumber={PRODUCT_CODE}",
        f"RevisionNumber={rev}",
        "OrderCode=FNT",
        "BaudRate_10=1", "BaudRate_20=1", "BaudRate_50=1", "BaudRate_125=1",
        "BaudRate_250=1", "BaudRate_500=1", "BaudRate_800=1", "BaudRate_1000=1",
        "SimpleBootUpMaster=0",
        "SimpleBootUpSlave=1",
        "Granularity=8",
        "DynamicChannelsSupported=0",
        "GroupMessaging=0",
        "NrOfRXPDO=2",
        "NrOfTXPDO=4",
        "LSS_Supported=0",
        "",
        "[DummyUsage]",
        "Dummy0001=0", "Dummy0002=0", "Dummy0003=0", "Dummy0004=0",
        "Dummy0005=0", "Dummy0006=0", "Dummy0007=0",
        "",
        "[Comments]",
        "Lines=3",
        f"Line1=Generated from dp_list.def for firmware {version}; indices 0x2000+i follow the catalog order.",
        "Line2=0x2FFF = number of datapoints, 0x3000+i = descriptor record (name/type/access) for EDS-less discovery.",
        "Line3=Writes run through the validated dp_write path (range, constraints, NVS). Passwords are write-only.",
        "",
        "[MandatoryObjects]",
        "SupportedObjects=3",
        "1=0x1000", "2=0x1001", "3=0x1018",
        "",
    ]))
    out.append(var(0x1000, "Device type", 0x0007, "ro", "0"))
    out.append(var(0x1001, "Error register", 0x0005, "ro", "0", pdo=1))
    out.append(record(0x1018, "Identity object", [
        ("Highest sub-index supported", 0x0005, "ro", "4", 0),
        ("Vendor-ID", 0x0007, "ro", "0", 0),
        ("Product code", 0x0007, "ro", str(PRODUCT_CODE), 0),
        ("Revision number", 0x0007, "ro", str(rev), 0),
        ("Serial number", 0x0007, "ro", "0", 0),
    ]))

    optional = [0x1005, 0x1008, 0x1009, 0x100A, 0x1014, 0x1017, 0x1200,
                0x1400, 0x1401, 0x1600, 0x1601,
                0x1800, 0x1801, 0x1802, 0x1803, 0x1A00, 0x1A01, 0x1A02, 0x1A03]
    out.append("[OptionalObjects]\nSupportedObjects=%d\n%s\n" % (
        len(optional), "\n".join(f"{i + 1}=0x{idx:04X}" for i, idx in enumerate(optional))))
    out.append(var(0x1005, "COB-ID SYNC message", 0x0007, "ro", "0x80"))
    out.append(var(0x1008, "Manufacturer device name", 0x0009, "const", "Fountainer"))
    out.append(var(0x1009, "Manufacturer hardware version", 0x0009, "const", ""))
    out.append(var(0x100A, "Manufacturer software version", 0x0009, "const", version))
    out.append(var(0x1014, "COB-ID EMCY", 0x0007, "ro", "$NODEID+0x80"))
    out.append(var(0x1017, "Producer heartbeat time", 0x0006, "rw", "1000"))
    out.append(record(0x1200, "SDO server parameter", [
        ("Highest sub-index supported", 0x0005, "ro", "2", 0),
        ("COB-ID client to server (rx)", 0x0007, "ro", "$NODEID+0x600", 0),
        ("COB-ID server to client (tx)", 0x0007, "ro", "$NODEID+0x580", 0),
    ]))
    for i in range(2):
        out.append(record(0x1400 + i, f"RPDO{i + 1} communication parameter", [
            ("Highest sub-index supported", 0x0005, "ro", "2", 0),
            ("COB-ID used by RPDO", 0x0007, "rw", f"$NODEID+0x{0x200 + 0x100 * i:X}", 0),
            ("Transmission type", 0x0005, "rw", "255", 0),
        ]))
    for i in range(2):
        out.append(record(0x1600 + i, f"RPDO{i + 1} mapping parameter",
                          [("Number of mapped objects", 0x0005, "rw", "0", 0)] +
                          [(f"Mapped object {k + 1}", 0x0007, "rw", "0", 0) for k in range(8)],
                          otype=0x8))
    for i in range(4):
        out.append(record(0x1800 + i, f"TPDO{i + 1} communication parameter", [
            ("Highest sub-index supported", 0x0005, "ro", "5", 0),
            ("COB-ID used by TPDO", 0x0007, "rw", f"$NODEID+0x{0x180 + 0x100 * i:X}", 0),
            ("Transmission type", 0x0005, "rw", "255", 0),
            ("Inhibit time", 0x0006, "rw", "0", 0),
            ("Reserved", 0x0005, "ro", "0", 0),
            ("Event timer", 0x0006, "rw", "0", 0),
        ]))
    for i in range(4):
        out.append(record(0x1A00 + i, f"TPDO{i + 1} mapping parameter",
                          [("Number of mapped objects", 0x0005, "rw", "0", 0)] +
                          [(f"Mapped object {k + 1}", 0x0007, "rw", "0", 0) for k in range(8)],
                          otype=0x8))

    mfr = [0x2000 + i for i in range(n)] + [0x2FFF] + [0x3000 + i for i in range(n)]
    out.append("[ManufacturerObjects]\nSupportedObjects=%d\n%s\n" % (
        len(mfr), "\n".join(f"{k + 1}=0x{idx:04X}" for k, idx in enumerate(mfr))))
    for i, row in enumerate(rows):
        _, dtype, size = DP_TYPES[row["type"]]
        if row["name"] in SECRET:
            access = "wo"
        else:
            access = {"RO": "ro", "RW": "rw", "WO": "wo"}[row["access"]]
        pdo = 0 if row["type"] == "STR" else 1
        out.append(var(0x2000 + i, row["name"], dtype, access, num_default(row), pdo))
    out.append(var(0x2FFF, "Datapoint count", 0x0006, "ro", str(n)))
    for i, row in enumerate(rows):
        code, _, _ = DP_TYPES[row["type"]]
        acc = ACCESS_CODE["WO" if row["name"] in SECRET else row["access"]]
        out.append(record(0x3000 + i, f"{row['name']} descriptor", [
            ("Highest sub-index supported", 0x0005, "ro", "3", 0),
            ("Name", 0x0009, "const", row["name"], 0),
            ("Type", 0x0005, "const", str(code), 0),
            ("Access", 0x0005, "const", str(acc), 0),
        ]))
    return "\n".join(out)


def build_json(rows, version):
    items = []
    for i, row in enumerate(rows):
        code, dtype, size = DP_TYPES[row["type"]]
        items.append({
            "name": row["name"], "index": 0x2000 + i, "descriptor_index": 0x3000 + i,
            "type": row["type"], "type_code": code, "cia_datatype": dtype,
            "access": "WO" if row["name"] in SECRET else row["access"],
            "persist": row["persist"],
            "unit": row["unit"], "dec": int(row["dec"]) if row["dec"] else None,
            "map": row["map"], "fmt": row["fmt"],
        })
    return {
        "firmware_version": version,
        "product_code": PRODUCT_CODE,
        "revision_number": sw_revision(version),
        "datapoint_count": len(rows),
        "index_base": 0x2000, "count_index": 0x2FFF, "descriptor_base": 0x3000,
        "default_tpdo": {
            "1": ["Fon_Current_Pressure", "Fon_Current_State", "Fon_Relay_Output", "Fon_Fault_Code"],
            "2": ["Fon_Pressure_Filtered", "Fon_Pressure_Slope"],
            "3": ["System_Uptime", "Fon_Run_Time"],
            "4": ["System_Temperature", "Net_Link_Score", "Fon_Demand_State",
                  "Fon_Starts_Per_Hour", "System_Power_Mode"],
        },
        "default_rpdo": {"1": ["Fon_Fault_Ack", "Fon_Event_Label"]},
        "datapoints": items,
    }


def write_if_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        old = path.read_text(encoding="utf-8")
        # ignore the volatile timestamp lines when deciding to rewrite
        strip = lambda s: "\n".join(l for l in s.splitlines()
                                    if not l.startswith(("CreationTime", "CreationDate",
                                                         "ModificationTime", "ModificationDate")))
        if strip(old) == strip(text):
            return False
    path.write_text(text, encoding="utf-8")
    return True


def main(argv):
    out_dir = OUT_DIR
    mirror = True
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--out":
            out_dir = Path(args.pop(0))
        elif a == "--no-mirror":
            mirror = False
    rows = parse_catalog()
    version = VERSION_FILE.read_text().strip()
    if not rows:
        sys.exit("gen_canopen_eds: no datapoints parsed")
    eds = build_eds(rows, version)
    js = json.dumps(build_json(rows, version), indent=1, ensure_ascii=False) + "\n"
    targets = [out_dir]
    if mirror and MIRROR_DIR.parent.is_dir():
        targets.append(MIRROR_DIR)
    for d in targets:
        w1 = write_if_changed(d / "fountainer.eds", eds)
        w2 = write_if_changed(d / "fountainer_od.json", js)
        print(f"gen_canopen_eds: {len(rows)} datapoints -> {d} "
              f"({'updated' if (w1 or w2) else 'unchanged'})")


main(sys.argv[1:] if STANDALONE else [])
