#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""Compatibility status API backed exclusively by the native control interface."""

import json
import os
import subprocess
import tempfile
from enum import Enum
from pathlib import Path
from typing import Dict, Any


class UnifiedStatus(str, Enum):
    """Native states plus the existing compatibility runtime-evidence states."""
    NOT_CONFIGURED = "NOT_CONFIGURED"
    SAVED = "SAVED"
    PENDING_RELOAD = "PENDING_RELOAD"
    APPLYING = "APPLYING"
    APPLIED = "APPLIED"
    REBOOT_REQUIRED = "REBOOT_REQUIRED"
    UNSUPPORTED = "UNSUPPORTED"
    INVALID_CONFIG = "INVALID_CONFIG"
    ENGINE_UNAVAILABLE = "ENGINE_UNAVAILABLE"
    APPLY_FAILED = "APPLY_FAILED"
    DISABLED = "DISABLED"
    INCOMPLETE_RUNTIME = "INCOMPLETE_RUNTIME"
    FAILED = "FAILED"
    READY = "READY"
    DEVICE_UNVERIFIED = "DEVICE_UNVERIFIED"


class NativeControlError(Exception):
    """The native control operation did not return a supported result."""


class StatusManager:
    """Reads current native evidence; never interprets raw ACKs or cached status."""

    ENGINES = ("hma", "pif", "tee", "copg", "kaorios")
    DEFAULT_CONTROL = Path("/data/adb/modules/kaorioshelper/helperctl.sh")

    def __init__(self, data_dir: Path, control_path: Path = None):
        self.data_dir = Path(data_dir).absolute()
        self.control_path = Path(control_path).absolute() if control_path is not None else self.DEFAULT_CONTROL
        self.status_file = self.data_dir / "status.json"

    def _run_control(self, *arguments: str) -> str:
        environment = dict(os.environ, KAORIOS_DATA_DIR=str(self.data_dir),
                           KAORIOS_MOD_DIR=str(self.data_dir.parent))
        try:
            result = subprocess.run([str(self.control_path), *arguments], env=environment,
                                    capture_output=True, text=True, timeout=30, check=True)
            return result.stdout
        except (OSError, subprocess.SubprocessError, UnicodeError) as error:
            # Command output may contain diagnostics; do not propagate it to callers.
            raise NativeControlError("Native control unavailable or operation failed") from error

    def _check_control(self) -> None:
        try:
            version = json.loads(self._run_control("version", "--json"))
            if (not isinstance(version, dict) or version.get("moduleId") != "kaorioshelper"
                    or type(version.get("protocolVersion")) is not int
                    or version["protocolVersion"] != 1
                    or type(version.get("statusSchemaVersion")) is not int
                    or version["statusSchemaVersion"] != 1
                    or not isinstance(version.get("capabilities"), dict)
                    or version["capabilities"].get("runtimeAckV2") is not True
                    or version["capabilities"].get("recoverableConfigTransactions") is not True):
                raise ValueError("Unsupported control contract")
        except (ValueError, TypeError) as error:
            raise NativeControlError("Native control contract unsupported") from error

    def _native_report(self) -> Dict[str, Any]:
        self._check_control()
        try:
            report = json.loads(self._run_control("status", "--json"))
            if (not isinstance(report, dict) or type(report.get("schemaVersion")) is not int
                    or report["schemaVersion"] != 1 or not isinstance(report.get("module"), dict)
                    or report["module"].get("id") != "kaorioshelper"
                    or type(report["module"].get("active")) is not bool
                    or not isinstance(report["module"].get("version"), str)
                    or not isinstance(report.get("engines"), dict)):
                raise ValueError("Invalid native report")
            for engine in self.ENGINES:
                state = report["engines"].get(engine)
                if not isinstance(state, dict) or state.get("status") not in UnifiedStatus._value2member_map_:
                    raise ValueError("Invalid native engine state")
                for field in ("enabled", "configValid", "runtimeAvailable", "pendingReload"):
                    if type(state.get(field)) is not bool:
                        raise ValueError("Invalid native engine flag")
                if state.get("deviceVerified") is not False:
                    raise ValueError("Unsupported device verification evidence")
                for field in ("storedGeneration", "requestedGeneration", "appliedGeneration"):
                    if type(state.get(field)) is not int or not 0 <= state[field] <= 2**64 - 1:
                        raise ValueError("Invalid native engine generation")
            return report
        except (ValueError, TypeError) as error:
            raise NativeControlError("Native control returned an invalid status report") from error

    def get_generation(self, engine_name: str, kind: str) -> int:
        """Read a native per-engine generation, not the legacy global counter."""
        if engine_name not in self.ENGINES or kind not in ("stored", "requested", "applied"):
            return 0
        return self.generate_report()["engines"][engine_name][kind + "Generation"]

    def get_runtime_ack_status(self, engine_name: str) -> UnifiedStatus:
        """APPLIED means native runtime initialization, never device verification."""
        state = self.get_engine_status(engine_name)
        if state in (UnifiedStatus.READY, UnifiedStatus.DEVICE_UNVERIFIED):
            return UnifiedStatus.APPLIED
        if state == UnifiedStatus.FAILED:
            return UnifiedStatus.APPLY_FAILED
        if state == UnifiedStatus.ENGINE_UNAVAILABLE:
            return state
        return UnifiedStatus.PENDING_RELOAD

    def get_engine_status(self, engine_name: str) -> UnifiedStatus:
        if engine_name not in self.ENGINES:
            return UnifiedStatus.ENGINE_UNAVAILABLE
        return UnifiedStatus(self.generate_report()["engines"][engine_name]["status"])

    def generate_report(self, module_version: str = None, root_mode: str = None) -> Dict[str, Any]:
        """Return the native wire report; legacy version/root-mode arguments are ignored."""
        try:
            return self._native_report()
        except NativeControlError:
            return {
                "schemaVersion": 1,
                "module": {"id": "kaorioshelper", "version": "unknown", "active": False},
                "engines": {engine: {
                    "status": UnifiedStatus.ENGINE_UNAVAILABLE.value,
                    "enabled": False, "configValid": False, "runtimeAvailable": False,
                    "deviceVerified": False, "pendingReload": True,
                    "storedGeneration": 0, "requestedGeneration": 0, "appliedGeneration": 0,
                    "lastError": "Native control unavailable or unsupported",
                } for engine in self.ENGINES},
            }

    def publish_status(self, module_version: str = None) -> None:
        """Publish a private atomic cache of one native report, never a status input."""
        report = self.generate_report(module_version)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.data_dir,
                                             prefix=".status.", delete=False) as stream:
                temporary = Path(stream.name)
                json.dump(report, stream, indent=2)
                stream.flush()
                os.fsync(stream.fileno())
            temporary.replace(self.status_file)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
