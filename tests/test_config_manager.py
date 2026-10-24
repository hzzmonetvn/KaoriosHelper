#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import os
import tempfile
import unittest
from pathlib import Path

from core.config.config_manager import (
    ConfigManager,
    ConfigValidationError,
    ConfigSecurityError,
    CONFIG_PIF,
    CONFIG_KEYBOX,
    CONFIG_SPOOF,
    CONFIG_HMA,
)


class TestConfigManager(unittest.TestCase):
    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.data_dir = Path(self.tmp_dir.name)
        self.mgr = ConfigManager(self.data_dir)

    def tearDown(self):
        self.tmp_dir.cleanup()

    def test_initial_generation(self):
        self.assertEqual(self.mgr.get_generation(), 0)

    def test_atomic_write_pif(self):
        valid_pif = """{
            "MANUFACTURER": "Google",
            "MODEL": "Pixel 7 Pro",
            "FINGERPRINT": "google/cheetah/cheetah:14/...",
            "spoofBuild": 1,
            "spoofProps": 1,
            "spoofProvider": 0
        }"""
        ok, msg, gen = self.mgr.write_config_atomic(CONFIG_PIF, valid_pif)
        self.assertTrue(ok)
        self.assertEqual(gen, 1)
        self.assertEqual(self.mgr.get_generation(), 1)

        ok_read, content = self.mgr.read_config(CONFIG_PIF)
        self.assertTrue(ok_read)
        self.assertIn("Pixel 7 Pro", content)

    def test_malformed_json_rejection(self):
        bad_json = '{"MANUFACTURER": "Google", unclosed'
        ok, msg, gen = self.mgr.write_config_atomic(CONFIG_PIF, bad_json)
        self.assertFalse(ok)
        self.assertIn("Validation failure", msg)
        self.assertEqual(gen, 0)
        self.assertFalse((self.data_dir / CONFIG_PIF).exists())

    def test_keybox_xml_validation_and_redaction(self):
        valid_keybox = """<?xml version="1.0"?>
        <AndroidAttestation>
            <NumberOfKeyboxes>1</NumberOfKeyboxes>
            <Keybox DeviceID="test">
                <Key algorithm="ecdsa">
                    <PrivateKey format="pem">
                        MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg...
                    </PrivateKey>
                    <Certificate format="pem">
                        -----BEGIN CERTIFICATE-----
                        MIIC...
                        -----END CERTIFICATE-----
                    </Certificate>
                </Key>
            </Keybox>
        </AndroidAttestation>"""
        ok, msg, gen = self.mgr.write_config_atomic(CONFIG_KEYBOX, valid_keybox)
        self.assertTrue(ok)
        self.assertEqual(gen, 1)

    def test_empty_or_invalid_keybox_rejected(self):
        invalid_xml = "<InvalidRoot><NoKeysHere/></InvalidRoot>"
        ok, msg, gen = self.mgr.write_config_atomic(CONFIG_KEYBOX, invalid_xml)
        self.assertFalse(ok)
        self.assertIn("Validation failure", msg)

    def test_symlink_rejection(self):
        # Create a symlink pointing to an outside file
        outside_file = self.data_dir / "outside.txt"
        outside_file.write_text("secret")
        symlink_target = self.data_dir / CONFIG_PIF
        os.symlink(outside_file, symlink_target)

        valid_pif = '{"MANUFACTURER": "Google"}'
        ok, msg, _ = self.mgr.write_config_atomic(CONFIG_PIF, valid_pif)
        self.assertFalse(ok)
        self.assertIn("Symlinks are prohibited", msg)

    def test_path_traversal_rejection(self):
        with self.assertRaises(ConfigSecurityError):
            self.mgr._resolve_safe_path("../../../etc/passwd")

    def test_rollback_to_last_known_good(self):
        # First write good config
        good_pif = '{"MANUFACTURER": "Google", "MODEL": "Pixel 7"}'
        self.mgr.write_config_atomic(CONFIG_PIF, good_pif)
        self.assertEqual(self.mgr.get_generation(), 1)

        # Second write updated good config
        good_pif2 = '{"MANUFACTURER": "Google", "MODEL": "Pixel 8"}'
        self.mgr.write_config_atomic(CONFIG_PIF, good_pif2)
        self.assertEqual(self.mgr.get_generation(), 2)

        # Rollback
        ok_rb, msg_rb = self.mgr.rollback(CONFIG_PIF)
        self.assertTrue(ok_rb)
        self.assertEqual(self.mgr.get_generation(), 3)

        _, read_back = self.mgr.read_config(CONFIG_PIF)
        self.assertIn("Pixel 7", read_back)


if __name__ == "__main__":
    unittest.main()
