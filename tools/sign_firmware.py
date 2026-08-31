# Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
# SPDX-License-Identifier: MIT
#
# PlatformIO post-script: signs firmware.bin with the OTA signing key
# (Secure Boot V2 RSA-3072 signature block, appended by espsecure).
#
# Needed because PlatformIO creates firmware.bin itself via esptool and
# BYPASSES the IDF build step that CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES
# would normally run. The running app verifies this block on every OTA
# ("Signed App Verification without Secure Boot" — pure software check,
# NO eFuses are burned).
#
# Key: testbed PKI ../DO_NOT_COMMIT/CA/ota_signing/ota_signing_key.pem (dummy,
# replace later). Missing key -> dev builds stay unsigned with a clear warning;
# the esp32s3_prod env aborts instead (an unsigned production image would be
# rejected by every device's OTA verification anyway).
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821  (PlatformIO SCons context)

KEY = Path(env["PROJECT_DIR"]) / ".." / "DO_NOT_COMMIT" / "CA" / "ota_signing" / "ota_signing_key.pem"  # noqa: F821


def sign_firmware(source, target, env):
    bin_path = Path(target[0].get_abspath())
    if not KEY.exists():
        if env["PIOENV"] == "esp32s3_prod":
            raise SystemExit(f"sign_firmware: ERROR — signing key {KEY} missing, production build aborted")
        print(f"sign_firmware: WARNING — signing key {KEY} missing, image stays UNSIGNED")
        return
    espsecure = Path(env.PioPlatform().get_package_dir("tool-esptoolpy")) / "espsecure.py"
    signed = bin_path.with_suffix(".signed.bin")
    subprocess.check_call([
        sys.executable, str(espsecure), "sign_data", "--version", "2",
        "--keyfile", str(KEY), "--output", str(signed), str(bin_path)])
    signed.replace(bin_path)          # in place: flash/OTA use the signed image
    print(f"sign_firmware: {bin_path.name} signed (RSA-3072 Secure Boot V2 block)")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", sign_firmware)  # noqa: F821
