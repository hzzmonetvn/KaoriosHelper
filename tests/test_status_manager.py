#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import json
import os
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

    def test_hma_and_pif_pending_when_only_configured(self):
        # Provide valid config files for hma and pif
        (self.data_dir / "hma.json").write_text('{"configVersion": 1}')
        (self.data_dir / "pif.json").write_text('{"MANUFACTURER": "Google"}')

        self.assertEqual(self.mgr.get_engine_status("hma"), UnifiedStatus.PENDING_RELOAD)
        self.assertEqual(self.mgr.get_engine_status("pif"), UnifiedStatus.PENDING_RELOAD)

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
        self.assertEqual(self.mgr.get_engine_status("tee"), UnifiedStatus.PENDING_RELOAD)

        # Mark reload pending
        (self.data_dir / "tee.reload_pending").touch()
        self.assertEqual(self.mgr.get_engine_status("tee"), UnifiedStatus.PENDING_RELOAD)

    def test_ack_evidence_and_generation_reporting(self):
        (self.data_dir / 'hma.json').write_text('{}')
        (self.data_dir / 'hma.stored_gen').write_text('2')
        (self.data_dir / 'hma.requested_gen').write_text('2')
        runtime = self.data_dir / 'runtime'
        runtime.mkdir()
        ack = dict(schemaVersion=1, protocolVersion=1, engine='hma',
                   processName='system_server', pid=123, uid=1000,
                   configGeneration=2, initResult=0, errorCode=0,
                   eventType='HOOK_INITIALIZED')
        cases = [({'configGeneration': 1}, UnifiedStatus.PENDING_RELOAD),
                 ({'configGeneration': 3}, UnifiedStatus.PENDING_RELOAD),
                 ({'engine': 'pif'}, UnifiedStatus.PENDING_RELOAD),
                 ({'protocolVersion': 2}, UnifiedStatus.PENDING_RELOAD),
                 ({'pid': 0}, UnifiedStatus.PENDING_RELOAD),
                 ({'uid': 2000}, UnifiedStatus.PENDING_RELOAD),
                 ({'initResult': -1}, UnifiedStatus.APPLY_FAILED),
                 ({'errorCode': 1}, UnifiedStatus.APPLY_FAILED),
                 ({'eventType': 'APPLY_FAILED'}, UnifiedStatus.APPLY_FAILED),
                 ({'eventType': 'HOOK_REGISTERED'}, UnifiedStatus.PENDING_RELOAD),
                 ({'configGeneration': True}, UnifiedStatus.PENDING_RELOAD),
                 ({'eventType': 'HOOK_OBSERVED'}, UnifiedStatus.APPLIED),
                 ({}, UnifiedStatus.APPLIED)]
        for changes, expected in cases:
            with self.subTest(changes=changes):
                (runtime / 'hma.ack.json').write_text(json.dumps({**ack, **changes}))
                self.assertEqual(self.mgr.get_engine_status('hma'), expected)
                report = self.mgr.generate_report()['engines']['hma']
                self.assertFalse(report['deviceVerified'])
                self.assertEqual(report['appliedGeneration'], 2 if expected == UnifiedStatus.APPLIED else 0)
        (self.data_dir / 'hma.requested_gen').write_text('3')
        self.assertEqual(self.mgr.get_engine_status('hma'), UnifiedStatus.PENDING_RELOAD)
        (self.data_dir / 'hma.requested_gen').write_text('2')
        (runtime / 'hma.ack.json').write_text('[]')
        self.assertEqual(self.mgr.get_engine_status('hma'), UnifiedStatus.PENDING_RELOAD)
        (runtime / 'hma.ack.json').write_text('{')
        self.assertEqual(self.mgr.get_engine_status('hma'), UnifiedStatus.PENDING_RELOAD)

    def test_ack_rejects_nonregular_symlink_and_writable_files(self):
        runtime = self.data_dir / 'runtime'
        runtime.mkdir()
        ack = runtime / 'hma.ack.json'
        os.mkfifo(ack)
        self.assertEqual(self.mgr.get_runtime_ack_status('hma'), UnifiedStatus.PENDING_RELOAD)
        ack.unlink()
        target = self.data_dir / 'external.json'
        target.write_text('{}')
        ack.symlink_to(target)
        self.assertEqual(self.mgr.get_runtime_ack_status('hma'), UnifiedStatus.PENDING_RELOAD)
        ack.unlink()
        ack.write_text('{}')
        ack.chmod(0o666)
        self.assertEqual(self.mgr.get_runtime_ack_status('hma'), UnifiedStatus.PENDING_RELOAD)
        ack.unlink()
        runtime.rmdir()
        other = self.data_dir / 'other-runtime'
        other.mkdir()
        runtime.symlink_to(other, target_is_directory=True)
        self.assertEqual(self.mgr.get_runtime_ack_status('hma'), UnifiedStatus.PENDING_RELOAD)
        self.assertEqual(self.mgr.get_engine_status('unknown'), UnifiedStatus.PENDING_RELOAD)

    def test_publish_status_writes_file(self):
        (self.data_dir / "generation.txt").write_text("5\n")
        self.mgr.publish_status("v1.2.3")
        status_file = self.data_dir / "status.json"
        self.assertTrue(status_file.is_file())

        data = json.loads(status_file.read_text())
        self.assertEqual(data["module"]["version"], "v1.2.3")
        self.assertEqual(data["module"]["activeGeneration"], 0)
        self.assertEqual(data["module"]["storedGeneration"], 5)


if __name__ == "__main__":
    unittest.main()
