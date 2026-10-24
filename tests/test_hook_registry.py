#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import unittest
from core.registry.hook_registry import HookRegistry, HookConflictError


class TestHookRegistry(unittest.TestCase):
    def setUp(self):
        self.registry = HookRegistry()

    def test_valid_hook_registration(self):
        self.registry.register_hook("pms_app_visibility", "hma")
        self.registry.register_hook("gms_system_properties", "pif")
        self.registry.register_hook("keystore_attestation", "tee")
        self.registry.register_hook("app_device_properties", "copg")

    def test_invalid_owner_rejected(self):
        # Attempting to assign PMS to COPG must fail
        with self.assertRaises(HookConflictError) as ctx:
            self.registry.register_hook("pms_app_visibility", "copg")
        self.assertIn("Ownership violation", str(ctx.exception))

    def test_unknown_hook_rejected(self):
        with self.assertRaises(HookConflictError):
            self.registry.register_hook("unknown_arbitrary_hook", "hma")

    def test_copg_gms_target_rejected(self):
        self.assertFalse(self.registry.validate_copg_target("com.google.android.gms"))
        self.assertFalse(self.registry.validate_copg_target("com.google.android.gms.unstable"))
        self.assertFalse(self.registry.validate_copg_target("com.android.vending"))
        self.assertFalse(self.registry.validate_copg_target("com.google.android.gms:auth"))
        # Regular games/apps should be allowed
        self.assertTrue(self.registry.validate_copg_target("com.tencent.tmgp.sgame"))
        self.assertTrue(self.registry.validate_copg_target("com.miHoYo.GenshinImpact"))

    def test_pif_tee_coexistence_validation(self):
        # When TEE is enabled, PIF spoofProvider must be 0
        valid, err = self.registry.validate_pif_tee_coexistence(pif_spoof_provider=0, tee_enabled=True)
        self.assertTrue(valid)
        self.assertIsNone(err)

        # When TEE is enabled and spoofProvider=1, conflict detected
        valid, err = self.registry.validate_pif_tee_coexistence(pif_spoof_provider=1, tee_enabled=True)
        self.assertFalse(valid)
        self.assertIn("Critical Hook Collision", err)

        # When TEE is disabled, spoofProvider=1 is allowed
        valid, err = self.registry.validate_pif_tee_coexistence(pif_spoof_provider=1, tee_enabled=False)
        self.assertTrue(valid)
        self.assertIsNone(err)


if __name__ == "__main__":
    unittest.main()
