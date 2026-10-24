#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import json
import tempfile
import unittest
from pathlib import Path

from core.status.status_manager import StatusManager, UnifiedStatus


class TestStatusManager(unittest.TestCase):
    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.data_dir = Path(self.tmp_dir.name)
        self.mgr = StatusManager(self.data_dir)

    def tearDown(self):
        self.tmp_dir.cleanup()

    def test_default_status_unconfigured(self):
        # By default without configs:
        # hma and pif have no config file -> NOT_CONFIGURED
        # tee and copg are opt-in and not enabled -> NOT_CONFIGURED
        report = self.mgr.generate_report("v1.0.0")
        self.assertEqual(report["module"]["version"], "v1.0.0")
        self.assertEqual(report["module"]["activeGeneration"], 0)
        self.assertFalse(report["module"]["rebootRequired"])

        for eng in ["hma", "pif", "tee", "copg"]:
            self.assertEqual(report["engines"][eng]["status"], UnifiedStatus.NOT_CONFIGURED.value)
            self.assertFalse(report["engines"][eng]["enabled"])

    def test_hma_and_pif_applied_when_configured(self):
        # Provide valid config files for hma and pif
        (self.data_dir / "hma.json").write_text('{"configVersion": 1}')
        (self.data_dir / "pif.json").write_text('{"MANUFACTURER": "Google"}')

        self.assertEqual(self.mgr.get_engine_status("hma"), UnifiedStatus.APPLIED)
        self.assertEqual(self.mgr.get_engine_status("pif"), UnifiedStatus.APPLIED)

        report = self.mgr.generate_report()
        self.assertTrue(report["engines"]["hma"]["enabled"])
        self.assertTrue(report["engines"]["pif"]["enabled"])

    def test_engine_disabled(self):
        (self.data_dir / "hma.json").write_text('{"configVersion": 1}')
        (self.data_dir / "hma.disabled").touch()
        self.assertEqual(self.mgr.get_engine_status("hma"), UnifiedStatus.ENGINE_UNAVAILABLE)

        report = self.mgr.generate_report()
        self.assertEqual(report["engines"]["hma"]["status"], UnifiedStatus.ENGINE_UNAVAILABLE.value)
        self.assertFalse(report["engines"]["hma"]["enabled"])

    def test_tee_copg_lifecycle(self):
        # TEE enabled marker without config
        (self.data_dir / "tee.enabled").touch()
        self.assertEqual(self.mgr.get_engine_status("tee"), UnifiedStatus.NOT_CONFIGURED)

        # Now add keybox
        (self.data_dir / "keybox.xml").write_text("<Keybox></Keybox>")
        self.assertEqual(self.mgr.get_engine_status("tee"), UnifiedStatus.APPLIED)

        # Mark reload pending
        (self.data_dir / "tee.reload_pending").touch()
        self.assertEqual(self.mgr.get_engine_status("tee"), UnifiedStatus.PENDING_RELOAD)

    def test_publish_status_writes_file(self):
        (self.data_dir / "generation.txt").write_text("5\n")
        self.mgr.publish_status("v1.2.3")
        status_file = self.data_dir / "status.json"
        self.assertTrue(status_file.is_file())

        data = json.loads(status_file.read_text())
        self.assertEqual(data["module"]["version"], "v1.2.3")
        self.assertEqual(data["module"]["activeGeneration"], 5)


if __name__ == "__main__":
    unittest.main()
