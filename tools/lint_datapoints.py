# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
#
# Build-time lint + UI-meta export for src/components/datapoints/dp_list.def
# (stages 1-3 of the project-adapted datapoints v2 plan).
#
# The C compiler already validates symbols/types of the X-macro list; this
# script enforces what the compiler cannot see:
#
#   L1  names unique, identifier charset, bounded length
#   L3  min <= max (numeric); NVS defaults inside min/max;
#       deadband >= 0 and only meaningful on F32
#   L4  class rules: RO+NVS only for the system-written Backup_* points;
#       STATIC never RW
#   L5  stable NVS ids: NVS points carry a unique id in 1..65534 that is
#       not on the RETIRED_IDS list; VOLATILE/STATIC carry 0.
#       (The former contiguity + layout-hash rules are OBSOLETE since the
#       per-key NVS store: position and layout no longer matter.)
#
# Additionally exports server/static/datapoints_meta.json for the web UI
# (type/access/persist + optional /*@ unit=.. dec=.. map=.. @*/ annotations)
# into the sibling fountainer_server checkout when present.
#
# Usage:
#   standalone:  python tools/lint_datapoints.py
#   PlatformIO:  extra_scripts = pre:tools/lint_datapoints.py  (fails build)

import json
import re
import sys
from pathlib import Path

# SCons executes pre-scripts via exec() WITHOUT __file__ — resolve the
# project root from the PlatformIO env there, from __file__ standalone.
try:
    ROOT = Path(__file__).resolve().parent.parent
except NameError:
    Import("env")  # noqa: F821  (PlatformIO SCons context)
    ROOT = Path(env["PROJECT_DIR"])  # noqa: F821

DEF_FILE = ROOT / "src/components/datapoints/dp_list.def"
META_OUT = ROOT.parent / "fountainer_server/server/static/datapoints_meta.json"

NUMERIC = {"U8", "U16", "U32", "U64", "I8", "I16", "I32", "F32", "ENUM"}
TYPES = NUMERIC | {"BOOL", "STR"}
MAX_NAME_LEN = 30

# NVS ids of REMOVED points — never reuse (their stored values may still
# exist on devices in the field).
RETIRED_IDS: set[int] = set()

DP_RE = re.compile(
    r"^\s*DP\(\s*([A-Za-z0-9_]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,"
    r"\s*(\d+)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^)\s]+)\s*\)"
    r"(?:.*?/\*@\s*(.*?)\s*@\*/)?")


def num(tok):
    tok = tok.strip().rstrip("fF")
    if tok.upper() == "NAN":
        return None
    try:
        return float(tok)
    except ValueError:
        return None


def fail(msg):
    print(f"dp_list.def LINT ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def parse():
    entries = []
    for lineno, line in enumerate(DEF_FILE.read_text().splitlines(), 1):
        if not line.strip().startswith("DP("):
            continue
        m = DP_RE.match(line)
        if not m:
            fail(f"line {lineno}: DP(...) line not parseable: {line.strip()[:60]}")
        name, typ, acc, per, nid, dv, mn, mx, db, anno = m.groups()
        meta = {}
        for tok in (anno or "").split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                meta[k] = v
        entries.append({
            "line": lineno, "name": name, "type": typ, "acc": acc, "per": per,
            "id": int(nid), "default": num(dv), "min": num(mn), "max": num(mx),
            "deadband": num(db), "meta": meta,
        })
    if not entries:
        fail("no DP() entries found — wrong path?")
    return entries


def lint(entries):
    seen_names, seen_ids = set(), set()
    for e in entries:
        n = e["name"]
        # L1
        if n in seen_names:
            fail(f"{n}: duplicate name")
        seen_names.add(n)
        if len(n) > MAX_NAME_LEN:
            fail(f"{n}: name longer than {MAX_NAME_LEN} chars")
        if e["type"] not in TYPES:
            fail(f"{n}: unknown type {e['type']}")
        if e["acc"] not in ("RO", "RW", "WO"):
            fail(f"{n}: unknown access {e['acc']}")
        if e["per"] not in ("VOLATILE", "NVS", "STATIC"):
            fail(f"{n}: unknown persist {e['per']}")
        # L3
        mn, mx, dv, db = e["min"], e["max"], e["default"], e["deadband"]
        if mn is not None and mx is not None and mn > mx:
            fail(f"{n}: min ({mn}) > max ({mx})")
        if e["per"] == "NVS" and e["type"] in NUMERIC and dv is not None:
            if mn is not None and dv < mn:
                fail(f"{n}: default {dv} below min {mn}")
            if mx is not None and dv > mx:
                fail(f"{n}: default {dv} above max {mx}")
        if db is not None and db < 0:
            fail(f"{n}: negative deadband")
        if db and db > 0 and e["type"] != "F32":
            fail(f"{n}: deadband only has effect on F32 (type is {e['type']})")
        # L4
        if e["per"] == "NVS" and e["acc"] == "RO" and not n.startswith("Backup_"):
            fail(f"{n}: RO+NVS is reserved for the system-written Backup_* set")
        if e["per"] == "STATIC" and e["acc"] != "RO":
            fail(f"{n}: STATIC points must be RO (set once from identity)")
        # L5 — stable NVS ids
        if e["per"] == "NVS":
            i = e["id"]
            if not (1 <= i <= 65534):
                fail(f"{n}: NVS id {i} outside 1..65534")
            if i in seen_ids:
                fail(f"{n}: duplicate NVS id {i}")
            if i in RETIRED_IDS:
                fail(f"{n}: NVS id {i} is RETIRED — never reuse ids")
            seen_ids.add(i)
        elif e["id"] != 0:
            fail(f"{n}: only NVS points carry an id (got {e['id']})")


def export_meta(entries):
    if not META_OUT.parent.is_dir():
        print(f"dp lint: meta export skipped ({META_OUT.parent} not present)")
        return
    meta = {}
    for e in entries:
        m = {"type": e["type"], "access": e["acc"], "persist": e["per"]}
        for k in ("unit", "dec", "map", "fmt"):
            if k in e["meta"]:
                m[k] = int(e["meta"][k]) if k == "dec" else e["meta"][k]
        meta[e["name"]] = m
    content = json.dumps(meta, indent=1)
    if not META_OUT.exists() or META_OUT.read_text() != content:
        META_OUT.write_text(content)
        print(f"dp lint: {META_OUT.name} exported ({len(meta)} points)")


def run():
    entries = parse()
    lint(entries)
    export_meta(entries)
    nvs = sum(1 for e in entries if e["per"] == "NVS")
    print(f"dp lint: OK — {len(entries)} datapoints ({nvs} NVS, per-key ids)")


if __name__ == "__main__":
    run()
else:
    run()
