#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import unittest
import zipfile
from pathlib import Path

from scripts.package_module import build_zip, MODULE_DIR, OUT_DIR

BINARIES_READY = (
    (MODULE_DIR / "helperctl").is_file()
    and (MODULE_DIR / "zygisk/arm64-v8a.so").is_file()
    and (MODULE_DIR / "zygisk/x86_64.so").is_file()
    and (MODULE_DIR / "zygisk/armeabi-v7a.so").is_file()
    and (MODULE_DIR / "zygisk/x86.so").is_file()
)


class TestPackageIntegration(unittest.TestCase):
    @unittest.skipUnless(
        BINARIES_READY,
        "Cross-compiled Android multi-ABI binaries not present in module/ (skipping host phase; runs in build-android)",
    )
    def test_full_cross_compiled_zip(self):
        zip_path = build_zip(require_binaries=True)
        self.assertTrue(zip_path.is_file())
        self.assertGreater(zip_path.stat().st_size, 50000)

        with zipfile.ZipFile(zip_path, "r") as zf:
            namelist = set(zf.namelist())
            required_entries = [
                "module.prop",
                "customize.sh",
                "post-fs-data.sh",
                "service.sh",
                "action.sh",
                "uninstall.sh",
                "helperctl.sh",
                "helperctl",
                "zygisk/arm64-v8a.so",
                "zygisk/x86_64.so",
                "zygisk/armeabi-v7a.so",
                "zygisk/x86.so",
                "bin/arm64-v8a/helperctl",
                "bin/x86_64/helperctl",
                "bin/armeabi-v7a/helperctl",
                "bin/x86/helperctl",
                "META-INF/com/google/android/update-binary",
                "META-INF/com/google/android/updater-script",
            ]
            for entry in required_entries:
                self.assertIn(entry, namelist, f"Missing required entry {entry} in official ZIP")

            for entry in zf.infolist():
                if (
                    entry.filename.endswith(".sh")
                    or "update-binary" in entry.filename
                    or entry.filename.endswith(".so")
                    or "helperctl" in entry.filename
                ):
                    mode = entry.external_attr >> 16
                    self.assertTrue(
                        mode & 0o111,
                        f"Executable permission bit missing on {entry.filename}",
                    )


if __name__ == "__main__":
    unittest.main()
