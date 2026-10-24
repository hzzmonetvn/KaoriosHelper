#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
KaoriosHelper Packaging Script.
Assembles the flashable Magisk/KernelSU/APatch ZIP module with appropriate permissions
and directory structure.
"""

import os
import sys
import zipfile
import shutil
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
MODULE_DIR = ROOT_DIR / "module"
OUT_DIR = ROOT_DIR / "out"


def get_module_version() -> str:
    prop_file = MODULE_DIR / "module.prop"
    if not prop_file.is_file():
        raise FileNotFoundError(f"Missing {prop_file}")

    for line in prop_file.read_text().splitlines():
        if line.startswith("version="):
            return line.split("=", 1)[1].strip()
    return "v1.0.0"


def build_zip() -> Path:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    version = get_module_version()
    zip_path = OUT_DIR / f"kaorioshelper-{version}.zip"

    # Files to include in the package
    required_files = [
        "module.prop",
        "customize.sh",
        "post-fs-data.sh",
        "service.sh",
        "action.sh",
        "uninstall.sh",
        "helperctl.sh",
        "helperctl",
        "zygisk/arm64-v8a.so",
        "META-INF/com/google/android/update-binary",
        "META-INF/com/google/android/updater-script",
    ]

    for rel_path in required_files:
        src = MODULE_DIR / rel_path
        if not src.is_file():
            raise FileNotFoundError(f"Required package file missing: {src}. Run scripts/build_android.sh first.")

    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(MODULE_DIR):
            for file in files:
                file_path = Path(root) / file
                rel_path = file_path.relative_to(MODULE_DIR)

                # Skip transient/temporary files or logs
                if file.startswith(".tmp") or file.endswith(".tmp") or ".old" in file:
                    continue

                # Add to ZIP preserving UNIX file attributes
                zinfo = zipfile.ZipInfo(str(rel_path))
                # Set permissions: 0755 for scripts/binaries, 0644 for others
                if file.endswith(".sh") or "update-binary" in file or file.endswith(".so") or file == "helperctl" or "bin/" in str(rel_path):
                    zinfo.external_attr = 0o755 << 16
                else:
                    zinfo.external_attr = 0o644 << 16

                # Copy content
                with open(file_path, "rb") as f:
                    zf.writestr(zinfo, f.read())

    return zip_path


if __name__ == "__main__":
    out = build_zip()
    print(f"Created package: {out}")
