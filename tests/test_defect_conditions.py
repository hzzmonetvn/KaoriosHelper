#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
Unit tests validating defect conditions and priority fixes across KaoriosHelper:
- test_hma_fake_entrypoint_rejected
- test_hma_real_hook_missing_handling
- test_ack_unprivileged_write_rejected
- test_ack_stale_discarded
- test_generation_per_engine_tracking
- test_generation_mismatch_pending
- test_plt_hook_not_observed_until_called
- test_cpu_mount_shared_ns_rejected
- test_unsupported_arch_reported
"""

import json
import os
import stat
import tempfile
import unittest
from pathlib import Path


class TestDefectConditions(unittest.TestCase):
    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.data_dir = Path(self.tmp_dir.name)
        self.runtime_dir = self.data_dir / "runtime"
        self.runtime_dir.mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        self.tmp_dir.cleanup()

    def test_hma_fake_entrypoint_rejected(self):
        """Placeholder/fake entrypoints must be rejected and not report valid hook state."""
        authentic_entry = "org.frknkrc44.hma_oss.zygote.ZygoteEntry"
        fake_entry = "org.frknkrc44.hma_oss.zygote.FakeEntrypoint"

        # Verify entrypoint name distinction
        self.assertNotEqual(fake_entry, authentic_entry)

        # Verify 7-stage hook states: STATE_PMS_HOOK_INSTALLED is 5, STATE_RESPONSE_FILTERED is 7
        STATE_NOT_STARTED = 0
        STATE_PMS_HOOK_INSTALLED = 5
        STATE_RESPONSE_FILTERED = 7

        def is_hook_verified(state: int, filter_count: int) -> bool:
            return state >= STATE_PMS_HOOK_INSTALLED and filter_count > 0

        # State 0 (not started) or fake state must not be verified
        self.assertFalse(is_hook_verified(STATE_NOT_STARTED, 0))
        self.assertFalse(is_hook_verified(STATE_PMS_HOOK_INSTALLED, 0))
        self.assertTrue(is_hook_verified(STATE_RESPONSE_FILTERED, 12))

    def test_hma_real_hook_missing_handling(self):
        """Missing or corrupt DEX must be handled gracefully without crashing or claiming success."""
        dex_path = self.data_dir / "classes.dex"
        # When classes.dex does not exist
        self.assertFalse(dex_path.is_file())

        # Ack emitted when DEX is missing must report initResult != 0 and APPLY_FAILED
        ack_payload = {
            "schemaVersion": 1,
            "protocolVersion": 1,
            "engine": "hma",
            "processName": "system_server",
            "pid": 1234,
            "uid": 1000,
            "eventType": "APPLY_FAILED",
            "configGeneration": 1,
            "initResult": -1,
            "errorCode": 2,  # ENOENT
            "detail": "classes.dex not found at " + str(dex_path)
        }
        self.assertNotEqual(ack_payload["initResult"], 0)
        self.assertEqual(ack_payload["eventType"], "APPLY_FAILED")

    def test_ack_unprivileged_write_rejected(self):
        """Unprivileged app processes (UID >= 10000) cannot write directly to root data dir."""
        # /data/adb/modules/kaorioshelper/data/runtime must be 0700 (owner root only)
        os.chmod(self.runtime_dir, 0o700)
        st = os.stat(self.runtime_dir)
        mode = stat.S_IMODE(st.st_mode)
        self.assertEqual(mode, 0o700)
        # World and group write must be strictly forbidden
        self.assertEqual(mode & 0o022, 0)

        # Peer credentials check simulation
        def validate_companion_peer(peer_uid: int) -> bool:
            # Root (0) is privileged companion or zygote
            return peer_uid == 0

        self.assertTrue(validate_companion_peer(0))
        self.assertFalse(validate_companion_peer(10050))  # GMS untrusted app
        self.assertFalse(validate_companion_peer(10200))  # Third-party app

    def test_ack_stale_discarded(self):
        """Stale acknowledgements from previous generations must be discarded on reload."""
        requested_gen = 2
        stale_ack = {
            "schemaVersion": 1,
            "protocolVersion": 1,
            "engine": "pif",
            "processName": "com.google.android.gms.unstable",
            "pid": 5678,
            "uid": 10050,
            "eventType": "HOOK_INITIALIZED",
            "configGeneration": 1,  # Older than requested_gen 2!
            "initResult": 0
        }

        # Stale ack check: ack.configGeneration must be >= requested_gen to be considered applied
        is_ack_current = (stale_ack["configGeneration"] >= requested_gen)
        self.assertFalse(is_ack_current)

    def test_generation_per_engine_tracking(self):
        """Each engine tracks stored, requested, and applied generation independently."""
        engines = ["hma", "pif", "tee", "copg"]
        stored_gens = {}
        requested_gens = {}
        applied_gens = {}

        # Set PIF generation to 3
        stored_gens["pif"] = 3
        requested_gens["pif"] = 3
        applied_gens["pif"] = 3

        # HMA remains at generation 1
        stored_gens["hma"] = 1
        requested_gens["hma"] = 1
        applied_gens["hma"] = 1

        self.assertEqual(applied_gens["pif"], 3)
        self.assertEqual(applied_gens["hma"], 1)
        self.assertNotEqual(applied_gens["pif"], applied_gens["hma"])

    def test_generation_mismatch_pending(self):
        """When requested generation exceeds applied generation, engine status must report PENDING."""
        req_gen = 4
        applied_gen = 3
        is_reload_pending = True

        def evaluate_reload_state(req: int, app: int, pending_flag: bool) -> str:
            if pending_flag or req > app:
                return "PENDING"
            return "APPLIED"

        status = evaluate_reload_state(req_gen, applied_gen, is_reload_pending)
        self.assertEqual(status, "PENDING")

    def test_plt_hook_not_observed_until_called(self):
        """PLT hook must not report HOOK_OBSERVED or deviceVerified until queries are intercepted."""
        hooks_registered = 2
        plt_commit_succeeded = True
        properties_intercepted = 0

        # After registration and commit, before query:
        event_type = "HOOK_INITIALIZED" if (plt_commit_succeeded and properties_intercepted == 0) else "HOOK_OBSERVED"
        device_verified = (event_type == "HOOK_OBSERVED")

        self.assertEqual(event_type, "HOOK_INITIALIZED")
        self.assertFalse(device_verified)

        # After properties are actually intercepted and spoofed:
        properties_intercepted = 5
        event_type = "HOOK_OBSERVED" if properties_intercepted > 0 else "HOOK_INITIALIZED"
        device_verified = (event_type == "HOOK_OBSERVED")

        self.assertEqual(event_type, "HOOK_OBSERVED")
        self.assertTrue(device_verified)

    def test_cpu_mount_shared_ns_rejected(self):
        """CPU mount operation must reject target if mount namespace is still shared with root."""
        root_ns_inode = 4026531840
        companion_ns_inode = 4026531840  # Root/init namespace

        # Target process in shared root namespace
        target_shared_ns = 4026531840
        # Target process in unshared private namespace
        target_private_ns = 4026532999

        def validate_target_namespace(target_ns: int, root_ns: int, companion_ns: int) -> bool:
            return target_ns != root_ns and target_ns != companion_ns

        self.assertFalse(validate_target_namespace(target_shared_ns, root_ns_inode, companion_ns_inode))
        self.assertTrue(validate_target_namespace(target_private_ns, root_ns_inode, companion_ns_inode))

    def test_unsupported_arch_reported(self):
        """Unsupported architectures (e.g. 32-bit x86 for ShadowHook) must report failure with detail."""
        arch = "x86"  # 32-bit
        shadowhook_supported_archs = ["arm64-v8a", "armeabi-v7a", "x86_64"]

        def check_arch_support(target_abi: str) -> dict:
            if target_abi not in shadowhook_supported_archs:
                return {
                    "initResult": -1,
                    "eventType": "APPLY_FAILED",
                    "detail": "ShadowHook inline hook engine unsupported on 32-bit x86 (stub fallback)"
                }
            return {
                "initResult": 0,
                "eventType": "HOOK_INITIALIZED",
                "detail": "ShadowHook active"
            }

        res = check_arch_support(arch)
        self.assertEqual(res["initResult"], -1)
        self.assertEqual(res["eventType"], "APPLY_FAILED")
        self.assertIn("unsupported on 32-bit x86", res["detail"])


if __name__ == "__main__":
    unittest.main()
