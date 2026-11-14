#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import json
import os
import unittest

from core.status.status_manager import StatusManager, UnifiedStatus
from native_control_support import NativeControlCase


class TestStatusManager(NativeControlCase):
    def setUp(self):
        super().setUp()
        self.mgr = StatusManager(self.data_dir, self.control)

    def test_native_report_is_not_reconstructed_from_disk(self):
        expected = json.loads(self.native("status", "--json").stdout)
        self.assertEqual(self.mgr.generate_report(), expected)
        self.assertEqual(self.mgr.generate_report("invented-version", "invented-mode"), expected)
        self.assertEqual(expected["module"]["version"], "v2.0.0-fixture")
        self.assertEqual(self.mgr.get_engine_status("hma"), UnifiedStatus.NOT_CONFIGURED)
        self.assertEqual(self.mgr.get_engine_status("copg"), UnifiedStatus.DISABLED)
        self.assertEqual(self.mgr.get_engine_status("unknown"), UnifiedStatus.ENGINE_UNAVAILABLE)

    def test_native_enablement_and_backend_states_are_preserved(self):
        self.configure("pif")
        self.assertEqual(self.mgr.get_engine_status("pif"), UnifiedStatus.PENDING_RELOAD)
        self.native("disable", "pif")
        self.assertEqual(self.mgr.get_engine_status("pif"), UnifiedStatus.DISABLED)
        self.configure("hma")
        self.assertEqual(self.mgr.get_engine_status("hma"), UnifiedStatus.INCOMPLETE_RUNTIME)
        self.configure("kaorios")
        self.assertEqual(self.mgr.get_engine_status("kaorios"), UnifiedStatus.INCOMPLETE_RUNTIME)
        xml = "<Keybox><Key><PrivateKey>fixture</PrivateKey><Certificate>fixture</Certificate></Key></Keybox>"
        self.configure("tee", xml)
        self.assertEqual(self.mgr.get_engine_status("tee"), UnifiedStatus.INCOMPLETE_RUNTIME)
        self.assertNotEqual(self.mgr.get_runtime_ack_status("tee"), UnifiedStatus.APPLIED)

    def test_live_native_producer_establishes_runtime_not_device_evidence(self):
        self.configure()
        process, _ = self.produce_ack()
        self.assertEqual(self.mgr.get_engine_status("copg"), UnifiedStatus.DEVICE_UNVERIFIED)
        self.assertEqual(self.mgr.get_runtime_ack_status("copg"), UnifiedStatus.APPLIED)
        self.assertEqual(self.mgr.get_generation("copg", "applied"), 1)
        report = self.mgr.generate_report()
        self.assertFalse(report["engines"]["copg"]["deviceVerified"])
        process.terminate()
        process.wait(timeout=10)
        self.assertEqual(self.mgr.get_runtime_ack_status("copg"), UnifiedStatus.PENDING_RELOAD)
        self.assertEqual(self.mgr.get_generation("copg", "applied"), 0)

    def test_legacy_stale_and_malformed_acks_stay_pending(self):
        self.configure()
        _, ack = self.produce_ack()
        path = self.data_dir / "runtime/copg.ack.json"
        for changes in ({"schemaVersion": 1, "protocolVersion": 1},
                        {"configGeneration": 2}, {"requestId": "f" * 32},
                        {"configHash": "0" * 64}, {"pid": 2147483647},
                        {"bootId": "00000000-0000-0000-0000-000000000000"},
                        {"processStartTicks": 1}, {"timestamp": 1}):
            with self.subTest(changes=changes):
                path.write_text(json.dumps({**ack, **changes}))
                self.assertEqual(self.mgr.get_runtime_ack_status("copg"), UnifiedStatus.PENDING_RELOAD)
                self.assertEqual(self.mgr.get_generation("copg", "applied"), 0)
        for payload in ("[]", "{", json.dumps(ack)[:-1] + ',"pid":1}'):
            path.write_text(payload)
            self.assertEqual(self.mgr.get_runtime_ack_status("copg"), UnifiedStatus.PENDING_RELOAD)

    def test_initialized_is_ready_without_claiming_observation(self):
        self.configure()
        self.produce_ack("HOOK_INITIALIZED")
        self.assertEqual(self.mgr.get_engine_status("copg"), UnifiedStatus.READY)
        self.assertEqual(self.mgr.get_runtime_ack_status("copg"), UnifiedStatus.APPLIED)
        state = self.mgr.generate_report()["engines"]["copg"]
        self.assertFalse(state["runtimeObserved"])
        self.assertFalse(state["deviceVerified"])

    def test_current_failure_is_not_applied(self):
        self.configure()
        self.produce_ack("APPLY_FAILED")
        self.assertEqual(self.mgr.get_engine_status("copg"), UnifiedStatus.FAILED)
        self.assertEqual(self.mgr.get_runtime_ack_status("copg"), UnifiedStatus.APPLY_FAILED)
        self.assertEqual(self.mgr.get_generation("copg", "applied"), 0)

    def test_native_ack_file_checks_are_used(self):
        self.configure()
        _, ack = self.produce_ack()
        path = self.data_dir / "runtime/copg.ack.json"
        for mode in ("fifo", "symlink", "writable"):
            with self.subTest(mode=mode):
                path.unlink()
                if mode == "fifo":
                    os.mkfifo(path)
                elif mode == "symlink":
                    target = self.base / "outside"
                    target.write_text(json.dumps(ack))
                    path.symlink_to(target)
                else:
                    path.write_text(json.dumps(ack))
                    path.chmod(0o666)
                self.assertEqual(self.mgr.get_runtime_ack_status("copg"), UnifiedStatus.PENDING_RELOAD)

    def test_missing_control_fails_closed_even_with_cached_applied_status(self):
        self.configure()
        self.produce_ack()
        self.mgr.publish_status()
        unavailable = StatusManager(self.data_dir, self.base / "missing-helperctl")
        report = unavailable.generate_report()
        self.assertFalse(report["module"]["active"])
        for state in report["engines"].values():
            self.assertEqual(state["status"], UnifiedStatus.ENGINE_UNAVAILABLE.value)
            self.assertFalse(state["enabled"])
            self.assertFalse(state["deviceVerified"])
            self.assertEqual(state["appliedGeneration"], 0)
        self.assertEqual(unavailable.get_generation("copg", "stored"), 0)
        self.assertEqual(unavailable.get_runtime_ack_status("copg"), UnifiedStatus.ENGINE_UNAVAILABLE)

    def test_old_control_contract_fails_closed(self):
        old_control = self.base / "old-helperctl"
        old_control.write_text('#!/bin/sh\nprintf \'%s\\n\' \'{"moduleId":"kaorioshelper","protocolVersion":1,"statusSchemaVersion":1,"capabilities":{"runtimeAckV2":false}}\'\n')
        old_control.chmod(0o700)
        old = StatusManager(self.data_dir, old_control)
        self.assertEqual(old.get_engine_status("copg"), UnifiedStatus.ENGINE_UNAVAILABLE)
        self.assertEqual(old.get_generation("copg", "applied"), 0)

    def test_native_control_failure_does_not_fall_back_to_raw_state(self):
        self.configure()
        self.produce_ack()
        lock = self.data_dir / ".config.lock"
        lock.unlink()
        target = self.base / "outside"
        target.write_text("untouched")
        lock.symlink_to(target)
        self.assertEqual(self.mgr.get_engine_status("copg"), UnifiedStatus.ENGINE_UNAVAILABLE)
        self.assertEqual(target.read_text(), "untouched")

    def test_publish_uses_native_report_and_private_atomic_cache(self):
        self.configure()
        self.produce_ack()
        (self.data_dir / "generation.txt").write_text("90\n")
        self.mgr.publish_status("not-the-native-version")
        cache = self.data_dir / "status.json"
        report = json.loads(cache.read_text())
        self.assertEqual(report, self.mgr.generate_report())
        self.assertEqual(report["module"]["version"], "v2.0.0-fixture")
        self.assertNotIn("storedGeneration", report["module"])
        self.assertEqual(report["engines"]["copg"]["storedGeneration"], 1)
        self.assertEqual(cache.stat().st_mode & 0o777, 0o600)
        self.assertEqual(list(self.data_dir.glob(".status.*")), [])


if __name__ == "__main__":
    unittest.main()
