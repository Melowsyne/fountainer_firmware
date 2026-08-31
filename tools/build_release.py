#!/usr/bin/env python3
# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
"""build_release.py — builds the shared, signed release build
(serienfertigung.md §15): one firmware.bin for all devices of a release.

    python3 tools/build_release.py [--env esp32s3_prod] [--allow-dirty]

Result: dist/<version>/release/{firmware.bin, bootloader.bin, partitions.bin,
ota_data_initial.bin, sha256.txt, build_manifest.json}.

PROTECTION RULE: writes EXCLUSIVELY to dist/ — never to
../fountainer_server/FIRMWARE_UPDATES (the server offers the global "latest"
to EVERY device; a test image there would go via OTA onto the operational device).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

FW_ROOT = Path(__file__).resolve().parent.parent
PIO = Path.home() / ".platformio" / "penv" / "bin" / "pio"
PENV_PY = Path.home() / ".platformio" / "penv" / "bin" / "python"
ESPSECURE = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "espsecure.py"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def embedded_version(bin_path: Path) -> str:
    """esp_app_desc_t.version: 32 bytes starting at offset 48 (0x30)."""
    with open(bin_path, "rb") as f:
        f.seek(48)
        return f.read(32).rstrip(b"\0").decode(errors="replace")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--env", default="esp32s3_prod")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="Build despite uncommitted changes (recorded in the manifest)")
    args = ap.parse_args()

    version = (FW_ROOT / "version.txt").read_text().strip()
    if not version:
        sys.exit("ERROR: version.txt is empty")

    dirty = subprocess.run(["git", "-C", str(FW_ROOT), "status", "--porcelain"],
                           capture_output=True, text=True).stdout.strip()
    if dirty and not args.allow_dirty:
        sys.exit("ERROR: working tree contains uncommitted changes.\n"
                 "Production build aborted (--allow-dirty to force).")
    git_sha = subprocess.run(["git", "-C", str(FW_ROOT), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or "nogit"
    if dirty:
        git_sha += "-dirty"

    # Pin the build version ONCE per release (§4): all devices identical.
    build_version = int(time.time() * 1000)
    env = dict(os.environ,
               FOUNTAINER_BUILD_VERSION=str(build_version),
               FOUNTAINER_BUILD_PROFILE="production")

    build_dir = FW_ROOT / ".pio" / "build" / args.env
    fw_bin = build_dir / "firmware.bin"

    run([PIO, "run", "-e", args.env], cwd=FW_ROOT, env=env)

    # Version guard (CMake reads version.txt only at configure time; PlatformIO
    # does not reconfigure reliably — guard as in build.sh:26-44).
    if embedded_version(fw_bin) != version:
        print(f"Version guard: image carries {embedded_version(fw_bin)!r}, "
              f"expected {version!r} -> clean rebuild")
        run([PIO, "run", "-e", args.env, "--target", "clean"], cwd=FW_ROOT)
        run([PIO, "run", "-e", args.env], cwd=FW_ROOT, env=env)
        if embedded_version(fw_bin) != version:
            sys.exit("ERROR: version in the image still wrong after clean rebuild")

    # Signature is mandatory (sign_firmware.py aborts for esp32s3_prod anyway;
    # this here is belt and braces).
    sig = subprocess.run([str(PENV_PY), str(ESPSECURE), "signature_info_v2", str(fw_bin)],
                         capture_output=True, text=True)
    if sig.returncode != 0:
        sys.exit(f"ERROR: firmware.bin carries no valid signature block:\n{sig.stdout}{sig.stderr}")

    # Collect artefacts — flash_device.py never depends on the build directory.
    dist = FW_ROOT / "dist" / version / "release"
    assert "FIRMWARE_UPDATES" not in str(dist.resolve()), "protection rule 1 violated"
    dist.mkdir(parents=True, exist_ok=True)
    artifacts = {}
    for name in ("firmware.bin", "bootloader.bin", "partitions.bin", "ota_data_initial.bin"):
        src = build_dir / name
        if not src.exists():
            sys.exit(f"ERROR: {src} missing")
        (dist / name).write_bytes(src.read_bytes())
        artifacts[name] = sha256(dist / name)
    # Carry along the flash parameters for the production station.
    flasher_args = build_dir / "flasher_args.json"
    flash_cfg = {}
    if flasher_args.exists():
        (dist / "flasher_args.json").write_bytes(flasher_args.read_bytes())
        fa = json.loads(flasher_args.read_text())
        flash_cfg = fa.get("flash_settings", {})

    idf_version = "unknown"
    pdesc = build_dir / "project_description.json"
    if pdesc.exists():
        idf_version = json.loads(pdesc.read_text()).get("idf_version", "unknown")
    pio_version = subprocess.run([str(PIO), "--version"], capture_output=True,
                                 text=True).stdout.strip()

    manifest = {
        "firmware_version": version,
        "build_version": build_version,
        "git_commit": git_sha,
        "environment": args.env,
        "profile": "production",
        "dirty_build": bool(dirty),
        "firmware": {"file": "firmware.bin", "sha256": artifacts["firmware.bin"]},
        "artifacts": artifacts,
        "flash_settings": flash_cfg,
        "toolchain": {
            "platformio": pio_version,
            "esp_idf": idf_version,
            "python": sys.version.split()[0],
        },
    }
    (dist / "build_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (dist / "sha256.txt").write_text(
        "".join(f"{h}  {n}\n" for n, h in sorted(artifacts.items())))

    print(f"\nRelease {version} (build {build_version}, git {git_sha}) -> {dist}")


if __name__ == "__main__":
    main()
