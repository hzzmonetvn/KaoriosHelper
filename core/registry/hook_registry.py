#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
KaoriosHelper HookRegistry.
Enforces single hook ownership, detects collisions between engines, and validates
process scoping to ensure boot safety and prevent double-spoofing.
"""

from typing import Dict, Set, Optional, Tuple


class HookConflictError(Exception):
    """Raised when an engine attempts to claim an already-owned hook or violates scoping."""
    pass


class HookRegistry:
    """Central registry tracking hook ownership and enforcing collision policies."""

    # Explicit ownership mapping
    PRIMARY_OWNERS = {
        "pms_app_visibility": "hma",
        "pms_installer_source": "hma",
        "settings_developer_status": "hma",
        "settings_adb_status": "hma",
        "gms_system_properties": "pif",
        "gms_build_fields": "pif",
        "keystore_attestation": "tee",
        "app_device_properties": "copg",
        "app_cpu_info": "copg",
        "app_android_id": "copg",
    }

    # Packages strictly reserved for PIF (COPG must never target these)
    PIF_RESERVED_PACKAGES = {
        "com.google.android.gms",
        "com.google.android.gms.unstable",
        "com.android.vending",
    }

    def __init__(self):
        self.registered_hooks: Dict[str, str] = {}
        self.active_processes: Dict[str, Set[str]] = {}

    def register_hook(self, hook_point: str, engine_name: str) -> None:
        """Registers ownership of a hook point, validating against primary owner policy."""
        expected_owner = self.PRIMARY_OWNERS.get(hook_point)
        if not expected_owner:
            raise HookConflictError(f"Unknown hook point: {hook_point}")

        if expected_owner != engine_name:
            raise HookConflictError(
                f"Ownership violation: Hook '{hook_point}' is exclusively owned by '{expected_owner}', "
                f"but '{engine_name}' attempted registration."
            )

        if hook_point in self.registered_hooks and self.registered_hooks[hook_point] != engine_name:
            raise HookConflictError(
                f"Collision: Hook '{hook_point}' already registered to '{self.registered_hooks[hook_point]}'"
            )

        self.registered_hooks[hook_point] = engine_name

    def validate_copg_target(self, package_name: str) -> bool:
        """Validates that a COPG target does not collide with PIF GMS reservation."""
        clean_pkg = package_name.split(":")[0].strip()
        for reserved in self.PIF_RESERVED_PACKAGES:
            if clean_pkg == reserved or clean_pkg.startswith("com.google.android.gms."):
                return False
        return True

    def validate_pif_tee_coexistence(self, pif_spoof_provider: int, tee_enabled: bool) -> Tuple[bool, Optional[str]]:
        """
        Validates that PIF spoofProvider is not active when TEE engine is enabled.
        Returns (valid: bool, error_message: Optional[str]).
        """
        if tee_enabled and pif_spoof_provider != 0:
            return False, (
                "Critical Hook Collision: PIF 'spoofProvider' is active (1) while TEE Engine is enabled. "
                "PIF spoofProvider must be set to 0 to prevent breaking Keystore hardware attestation simulation."
            )
        return True, None
