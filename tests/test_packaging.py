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
        self.assertIn("author=HzzMonet", content)
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
                    self.assertEqual(
                        mode & 0o170000,
                        0o100000,
                        f"Expected S_IFREG regular file bits for {entry.filename}",
                    )
                    self.assertEqual(
                        entry.compress_type,
                        zipfile.ZIP_DEFLATED,
                        f"Expected ZIP_DEFLATED compression for {entry.filename}",
                    )
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


    def test_boot_rejects_runtime_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            module = base / "module"
            (module / "data").mkdir(parents=True)
            outside = base / "outside"
            outside.mkdir()
            victim = outside / "unrelated.json"
            victim.write_text("preserve")
            (module / "data" / "runtime").symlink_to(outside, target_is_directory=True)
            script = module / "post-fs-data.sh"
            script.write_text((MODULE_DIR / "post-fs-data.sh").read_text())
            result = subprocess.run(["sh", str(script)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(victim.read_text(), "preserve")

    def test_boot_prepares_request_and_action_syncs_before_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            module = Path(tmp)
            recorder = module / 'calls'
            control = module / 'helperctl.sh'
            control.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "${0%/*}/calls"\nexit "${CONTROL_EXIT:-0}"\n')
            for name in ('post-fs-data.sh', 'action.sh'):
                (module / name).write_text((MODULE_DIR / name).read_text())
            boot = subprocess.run(['sh', str(module / 'post-fs-data.sh')], capture_output=True)
            self.assertEqual(boot.returncode, 0, boot.stderr)
            self.assertEqual(recorder.read_text().splitlines(), ['prepare-boot'])
            action = subprocess.run(['sh', str(module / 'action.sh')], capture_output=True)
            self.assertEqual(action.returncode, 0, action.stderr)
            self.assertEqual(recorder.read_text().splitlines(), ['prepare-boot', 'config sync all', 'status'])
            failed = subprocess.run(['sh', str(module / 'action.sh')], capture_output=True,
                                    env=dict(os.environ, CONTROL_EXIT='7'))
            self.assertEqual(failed.returncode, 7)
            self.assertEqual(recorder.read_text().splitlines()[-1], 'config sync all')

    def test_packaging_excludes_runtime_data(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as out:
            import shutil
            source = Path(tmp) / "module"
            shutil.copytree(MODULE_DIR, source)
            (source / "data" / "keybox.xml").write_text("PRIVATE TEST MATERIAL")
            package = build_zip(source, Path(out), require_binaries=False)
            with zipfile.ZipFile(package) as archive:
                self.assertFalse(any(name.startswith("data/") for name in archive.namelist()))
                self.assertFalse(any(b"PRIVATE TEST MATERIAL" in archive.read(name)
                                     for name in archive.namelist()))

    def test_post_fs_data_resetprop_rules(self):
        post_fs_data = MODULE_DIR / "post-fs-data.sh"
        self.assertTrue(post_fs_data.is_file())
        content = post_fs_data.read_text()
        # Boot preparation must leave global security properties untouched.
        self.assertNotIn("resetprop", content)
        self.assertNotIn("resetprop", (MODULE_DIR / "service.sh").read_text())
        self.assertIn("chmod 0700", content)

    def test_customize_sh_safe_when_sourced_by_kernelsu_installer(self):
        customize_path = MODULE_DIR / "customize.sh"
        script_text = customize_path.read_text()
        for line in script_text.splitlines():
            stripped = line.split("#", 1)[0].strip()
            self.assertNotRegex(stripped, r"(?:^|\s)set\s+-[A-Za-z]*[eu]")

        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            modpath = base / "modpath"
            (modpath / "bin" / "arm64-v8a").mkdir(parents=True)
            (modpath / "zygisk").mkdir(parents=True)
            (modpath / "bin" / "arm64-v8a" / "helperctl").write_text("#!/bin/sh\nexit 0\n")
            (modpath / "classes.dex").write_bytes(b"dex\n039\x00")
            (modpath / "zygisk" / "arm64-v8a.so").write_bytes(b"\x7fELF")
            for sh_name in (
                "customize.sh",
                "post-fs-data.sh",
                "service.sh",
                "uninstall.sh",
                "action.sh",
                "helperctl.sh",
            ):
                (modpath / sh_name).write_text("#!/bin/sh\n")
            (modpath / "customize.sh").write_text(script_text)

            backup_dir = base / "backup"
            harness = "\n".join(
                [
                    'MODPATH="$1"',
                    "ui_print() { :; }",
                    "abort() { echo \"$*\" >&2; exit 1; }",
                    "getprop() {",
                    "  case \"$1\" in",
                    "    ro.build.version.sdk) echo 37 ;;",
                    "    ro.product.cpu.abi) echo arm64-v8a ;;",
                    "    ro.product.cpu.abilist) echo arm64-v8a,armeabi-v7a ;;",
                    "  esac",
                    "}",
                    "set_perm() { chmod \"$4\" \"$1\"; }",
                    "unset REPLACE REMOVE",
                    "KSU=true",
                    "export KSU",
                    ". \"$MODPATH/customize.sh\"",
                    "for target in $REPLACE; do exit 99; done",
                    "for target in $REMOVE; do exit 98; done",
                ]
            )
            env = {
                "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                "KAORIOS_CONFIG_DIR": str(base / "existing_cfg"),
                "KAORIOS_LEGACY_DIR": str(base / "legacy_cfg"),
                "KAORIOS_BACKUP_DIR": str(backup_dir),
            }
            res = subprocess.run(
                ["sh", "-c", harness, "installer.sh", str(modpath)],
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, f"stderr: {res.stderr}\nstdout: {res.stdout}")
            self.assertEqual((modpath / "helperctl").stat().st_mode & 0o777, 0o755)
            self.assertEqual((modpath / "classes.dex").stat().st_mode & 0o777, 0o644)
            self.assertEqual((modpath / "zygisk" / "arm64-v8a.so").stat().st_mode & 0o777, 0o644)
            self.assertTrue((modpath / "data" / "hma.json").is_file())
            self.assertFalse(backup_dir.exists())


if __name__ == "__main__":
    unittest.main()

