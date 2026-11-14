#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import concurrent.futures
import json
import unittest

from core.runtime.runtime_manager import RuntimeManager
from core.status.status_manager import UnifiedStatus
from native_control_support import NativeControlCase


class TestRuntimeManager(NativeControlCase):
    def setUp(self):
        super().setUp()
        self.mgr = RuntimeManager(self.data_dir, self.control)

    def test_default_enabled_states_come_from_native_control(self):
        self.assertTrue(self.mgr.is_engine_enabled("hma"))
        self.assertTrue(self.mgr.is_engine_enabled("pif"))
        self.assertFalse(self.mgr.is_engine_enabled("tee"))
        self.assertFalse(self.mgr.is_engine_enabled("copg"))
        self.assertFalse(self.mgr.is_engine_enabled("unknown"))

    def test_enable_disable_uses_native_transactions_and_new_requests(self):
        ok, message = self.mgr.enable_engine("copg")
        self.assertTrue(ok, message)
        self.assertIn("accepted", message)
        self.assertIn("pending", message)
        self.assertTrue(self.mgr.is_engine_enabled("copg"))
        self.assertTrue((self.data_dir / "copg.enabled").is_file())
        first = json.loads((self.data_dir / "copg.request.json").read_text())
        self.assertEqual((self.data_dir / "copg.requested_gen").read_text(), "0\n")
        ok, message = self.mgr.disable_engine("copg")
        self.assertTrue(ok, message)
        self.assertFalse(self.mgr.is_engine_enabled("copg"))
        self.assertFalse((self.data_dir / "copg.enabled").exists())
        second = json.loads((self.data_dir / "copg.request.json").read_text())
        self.assertNotEqual(first["requestId"], second["requestId"])
        self.assertTrue((self.data_dir / "copg.reload_pending").exists())

    def test_disable_enable_default_engine(self):
        self.assertTrue(self.mgr.disable_engine("hma")[0])
        self.assertFalse(self.mgr.is_engine_enabled("hma"))
        self.assertTrue((self.data_dir / "hma.disabled").is_file())
        self.assertTrue(self.mgr.enable_engine("hma")[0])
        self.assertTrue(self.mgr.is_engine_enabled("hma"))
        self.assertFalse((self.data_dir / "hma.disabled").exists())

    def test_reload_acceptance_is_not_runtime_application(self):
        self.configure()
        process, previous_ack = self.produce_ack()
        self.assertEqual(self.mgr.status_mgr.get_runtime_ack_status("copg"), UnifiedStatus.APPLIED)
        generation = self.mgr.status_mgr.get_generation("copg", "stored")
        ok, message = self.mgr.trigger_reload("copg")
        self.assertTrue(ok, message)
        self.assertIn("request accepted", message)
        self.assertIn("application pending", message)
        current = json.loads((self.data_dir / "copg.request.json").read_text())
        self.assertNotEqual(previous_ack["requestId"], current["requestId"])
        self.assertEqual(self.mgr.status_mgr.get_generation("copg", "stored"), generation)
        self.assertEqual(self.mgr.status_mgr.get_generation("copg", "requested"), generation)
        self.assertEqual(self.mgr.status_mgr.get_generation("copg", "applied"), 0)
        self.assertEqual(self.mgr.status_mgr.get_runtime_ack_status("copg"), UnifiedStatus.PENDING_RELOAD)
        self.assertTrue((self.data_dir / "copg.reload_pending").exists())
        process.terminate()
        process.wait(timeout=10)
        self.produce_ack()
        self.assertEqual(self.mgr.status_mgr.get_runtime_ack_status("copg"), UnifiedStatus.APPLIED)
        self.assertFalse(self.mgr.status_mgr.generate_report()["engines"]["copg"]["deviceVerified"])

    def test_legacy_ack_does_not_clear_reload_pending(self):
        self.configure()
        runtime = self.data_dir / "runtime"
        runtime.mkdir()
        (runtime / "copg.ack.json").write_text(json.dumps({
            "schemaVersion": 1, "protocolVersion": 1, "engine": "copg",
            "processName": "legacy-fixture", "pid": 123, "uid": 0,
            "eventType": "HOOK_OBSERVED", "configGeneration": 1,
            "initResult": 0, "errorCode": 0, "timestamp": 1, "detail": "fixture",
        }))
        self.assertTrue(self.mgr.trigger_reload("copg")[0])
        self.assertEqual(self.mgr.status_mgr.get_runtime_ack_status("copg"), UnifiedStatus.PENDING_RELOAD)
        self.assertTrue((self.data_dir / "copg.reload_pending").exists())

    def test_unavailable_native_control_leaves_runtime_files_unchanged(self):
        self.configure()
        self.produce_ack()
        before = {p.name: p.read_bytes() for p in self.data_dir.iterdir() if p.is_file()}
        unavailable = RuntimeManager(self.data_dir, self.base / "missing-helperctl")
        self.assertFalse(unavailable.is_engine_enabled("copg"))
        for request in (unavailable.enable_engine, unavailable.disable_engine, unavailable.trigger_reload):
            self.assertFalse(request("copg")[0])
        after = {p.name: p.read_bytes() for p in self.data_dir.iterdir() if p.is_file()}
        self.assertEqual(after, before)

    def test_native_mutation_error_does_not_fall_back_to_marker_writes(self):
        outside = self.base / "outside"
        outside.write_text("untouched")
        (self.data_dir / ".config.lock").symlink_to(outside)
        self.assertFalse(self.mgr.enable_engine("copg")[0])
        self.assertFalse((self.data_dir / "copg.enabled").exists())
        self.assertFalse((self.data_dir / "copg.request.json").exists())
        self.assertEqual(outside.read_text(), "untouched")

    def test_concurrent_requests_do_not_truncate_shared_metadata(self):
        self.configure()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.mgr.trigger_reload("copg"), range(8)))
        self.assertTrue(all(ok for ok, _ in results), results)
        report = self.mgr.status_mgr.generate_report()["engines"]["copg"]
        self.assertEqual(report["storedGeneration"], 1)
        self.assertEqual(report["requestedGeneration"], 1)
        self.assertEqual(report["appliedGeneration"], 0)
        self.assertTrue(report["pendingReload"])
        request = json.loads((self.data_dir / "copg.request.json").read_text())
        self.assertRegex(request["requestId"], "^[0-9a-f]{32}$")
        self.assertFalse((self.data_dir / ".native-transaction.json").exists())

    def test_invalid_engine_is_not_forwarded_as_a_command(self):
        for engine in ("invalid_engine", "copg; touch injected", "--json"):
            for operation in (self.mgr.enable_engine, self.mgr.disable_engine, self.mgr.trigger_reload):
                ok, message = operation(engine)
                self.assertFalse(ok)
                self.assertIn("Unknown engine", message)
        self.assertEqual(list(self.data_dir.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
