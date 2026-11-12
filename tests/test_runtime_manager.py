#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import json
import tempfile
import unittest
from pathlib import Path

from core.runtime.runtime_manager import RuntimeManager
from core.status.status_manager import UnifiedStatus


class TestRuntimeManager(unittest.TestCase):
    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.data_dir = Path(self.tmp_dir.name)
        self.mgr = RuntimeManager(self.data_dir)

    def tearDown(self):
        self.tmp_dir.cleanup()

    def test_default_enabled_states(self):
        # hma and pif enabled by default
        self.assertTrue(self.mgr.is_engine_enabled("hma"))
        self.assertTrue(self.mgr.is_engine_enabled("pif"))
        # tee and copg disabled by default
        self.assertFalse(self.mgr.is_engine_enabled("tee"))
        self.assertFalse(self.mgr.is_engine_enabled("copg"))

    def test_enable_disable_opt_in_engine(self):
        # Enable tee
        ok, msg = self.mgr.enable_engine("tee")
        self.assertTrue(ok)
        self.assertTrue(self.mgr.is_engine_enabled("tee"))
        self.assertTrue((self.data_dir / "tee.enabled").is_file())
        self.assertTrue((self.data_dir / "tee.reload_pending").is_file())

        # No runtime acknowledgement: preserve the pending request.
        ok_rel, _ = self.mgr.trigger_reload("tee")
        self.assertFalse(ok_rel)
        self.assertTrue((self.data_dir / "tee.reload_pending").is_file())

        # Disable tee
        ok_dis, _ = self.mgr.disable_engine("tee")
        self.assertTrue(ok_dis)
        self.assertFalse(self.mgr.is_engine_enabled("tee"))
        self.assertFalse((self.data_dir / "tee.enabled").is_file())

    def test_disable_enable_default_engine(self):
        # Disable hma
        ok, _ = self.mgr.disable_engine("hma")
        self.assertTrue(ok)
        self.assertFalse(self.mgr.is_engine_enabled("hma"))
        self.assertTrue((self.data_dir / "hma.disabled").is_file())

        # Enable hma
        ok_en, _ = self.mgr.enable_engine("hma")
        self.assertTrue(ok_en)
        self.assertTrue(self.mgr.is_engine_enabled("hma"))
        self.assertFalse((self.data_dir / "hma.disabled").is_file())

    def test_reload_requires_current_successful_exact_engine_ack(self):
        (self.data_dir / "hma.json").write_text('{}')
        (self.data_dir / "hma.stored_gen").write_text('2')
        (self.data_dir / "hma.requested_gen").write_text('2')
        runtime = self.data_dir / "runtime"
        runtime.mkdir()
        ack = dict(schemaVersion=1, protocolVersion=1, engine='hma',
                   processName='system_server', pid=123, uid=1000,
                   eventType='HOOK_INITIALIZED', configGeneration=2,
                   initResult=0, errorCode=0)
        for changes in ({'configGeneration': 1}, {'configGeneration': 3},
                        {'engine': 'pif'}, {'eventType': 'APPLY_FAILED'},
                        {'initResult': -1}, {'errorCode': 1},
                        {'eventType': 'CONFIG_LOADED'}, {}):
            with self.subTest(changes=changes):
                (runtime / 'hma.ack.json').write_text(json.dumps({**ack, **changes}))
                ok, message = self.mgr.trigger_reload('hma')
                self.assertEqual(ok, not changes)
                self.assertEqual((self.data_dir / 'hma.reload_pending').exists(), bool(changes))
                if ok:
                    self.assertIn('device unverified', message)

    def test_invalid_engine_handling(self):
        ok, msg = self.mgr.enable_engine("invalid_engine")
        self.assertFalse(ok)
        self.assertIn("Unknown engine", msg)


if __name__ == "__main__":
    unittest.main()
