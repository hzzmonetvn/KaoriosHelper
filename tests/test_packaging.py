#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import os
import subprocess
import tempfile
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

    def test_packaging_fixture_logic(self):
        # Test packaging logic using a clean temporary fixture without requiring pre-compiled binaries
        with tempfile.TemporaryDirectory() as tmp_mod_dir, tempfile.TemporaryDirectory() as tmp_out_dir:
            mod_path = Path(tmp_mod_dir)
            out_path = Path(tmp_out_dir)

            # Create mock module tree
            (mod_path / "module.prop").write_text("id=kaorioshelper\nversion=v1.0.0-test\n")
            for req in [
                "customize.sh",
                "post-fs-data.sh",
                "service.sh",
                "action.sh",
                "uninstall.sh",
                "helperctl.sh",
            ]:
                (mod_path / req).write_text("#!/bin/sh\nexit 0\n")

            meta_dir = mod_path / "META-INF/com/google/android"
            meta_dir.mkdir(parents=True)
            (meta_dir / "update-binary").write_text("#!/bin/sh\n")
            (meta_dir / "updater-script").write_text("# dummy\n")

            # Add a transient file that must be excluded
            (mod_path / "cache.tmp").write_text("transient")
            # Add a regular data file
            (mod_path / "README.md").write_text("info")

            zip_res = build_zip(module_dir=mod_path, out_dir=out_path, require_binaries=False)
            self.assertTrue(zip_res.is_file())

            with zipfile.ZipFile(zip_res, "r") as zf:
                names = zf.namelist()
                self.assertIn("module.prop", names)
                self.assertIn("customize.sh", names)
                self.assertIn("META-INF/com/google/android/update-binary", names)
                self.assertIn("README.md", names)
                self.assertNotIn("cache.tmp", names)

                # Verify permissions inside ZIP
                for entry in zf.infolist():
                    mode = entry.external_attr >> 16
                    if entry.filename.endswith(".sh") or "update-binary" in entry.filename:
                        self.assertEqual(
                            mode & 0o755,
                            0o755,
                            f"Expected executable 0755 permissions for {entry.filename}",
                        )
                    elif entry.filename == "README.md":
                        self.assertEqual(
                            mode & 0o777,
                            0o644,
                            f"Expected 0644 permissions for {entry.filename}",
                        )

    def test_missing_required_file_raises(self):
        with tempfile.TemporaryDirectory() as tmp_mod_dir, tempfile.TemporaryDirectory() as tmp_out_dir:
            mod_path = Path(tmp_mod_dir)
            out_path = Path(tmp_out_dir)
            (mod_path / "module.prop").write_text("id=kaorioshelper\nversion=v1.0.0-test\n")
            # Missing scripts
            with self.assertRaises(FileNotFoundError):
                build_zip(module_dir=mod_path, out_dir=out_path, require_binaries=False)

    def test_missing_binaries_raises_when_required(self):
        with tempfile.TemporaryDirectory() as tmp_mod_dir, tempfile.TemporaryDirectory() as tmp_out_dir:
            mod_path = Path(tmp_mod_dir)
            out_path = Path(tmp_out_dir)
            (mod_path / "module.prop").write_text("id=kaorioshelper\nversion=v1.0.0-test\n")
            for req in [
                "customize.sh",
                "post-fs-data.sh",
                "service.sh",
                "action.sh",
                "uninstall.sh",
                "helperctl.sh",
            ]:
                (mod_path / req).write_text("#!/bin/sh\nexit 0\n")
            meta_dir = mod_path / "META-INF/com/google/android"
            meta_dir.mkdir(parents=True)
            (meta_dir / "update-binary").write_text("#!/bin/sh\n")
            (meta_dir / "updater-script").write_text("# dummy\n")

            with self.assertRaises(FileNotFoundError):
                build_zip(module_dir=mod_path, out_dir=out_path, require_binaries=True)


if __name__ == "__main__":
    unittest.main()

