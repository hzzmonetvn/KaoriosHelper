#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import os
import subprocess
import unittest
import zipfile
from pathlib import Path

from scripts.package_module import build_zip, get_module_version, MODULE_DIR


class TestPackaging(unittest.TestCase):
    def test_module_prop_spec(self):
        prop_file = MODULE_DIR / "module.prop"
        self.assertTrue(prop_file.is_file())
        content = prop_file.read_text()
        self.assertIn("id=kaorioshelper", content)
        self.assertIn("name=KaoriosHelper", content)
        self.assertIn("author=HzzMonet / Kaorios Project", content)
        self.assertIn("minMagisk=26400", content)

    def test_shell_scripts_syntax(self):
        # Validate that all shell scripts pass `sh -n` syntax check
        sh_scripts = list(MODULE_DIR.glob("**/*.sh")) + [
            MODULE_DIR / "META-INF/com/google/android/update-binary"
        ]
        self.assertGreater(len(sh_scripts), 4)

        for script in sh_scripts:
            self.assertTrue(script.is_file())
            result = subprocess.run(["sh", "-n", str(script)], capture_output=True, text=True)
            self.assertEqual(
                result.returncode,
                0,
                f"Syntax check failed for {script.name}:\n{result.stderr}",
            )

    def test_build_zip_archive(self):
        zip_path = build_zip()
        self.assertTrue(zip_path.is_file())
        self.assertGreater(zip_path.stat().st_size, 1000)

        with zipfile.ZipFile(zip_path, "r") as zf:
            namelist = zf.namelist()
            self.assertIn("module.prop", namelist)
            self.assertIn("customize.sh", namelist)
            self.assertIn("post-fs-data.sh", namelist)
            self.assertIn("service.sh", namelist)
            self.assertIn("action.sh", namelist)
            self.assertIn("uninstall.sh", namelist)
            self.assertIn("helperctl.sh", namelist)
            self.assertIn("META-INF/com/google/android/update-binary", namelist)
            self.assertIn("META-INF/com/google/android/updater-script", namelist)

            # Check permissions attribute on executable files
            for entry in zf.infolist():
                if entry.filename.endswith(".sh") or "update-binary" in entry.filename:
                    mode = entry.external_attr >> 16
                    self.assertTrue(mode & 0o111, f"Executable permission missing on {entry.filename}")


if __name__ == "__main__":
    unittest.main()
