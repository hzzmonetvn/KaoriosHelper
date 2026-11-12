#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
KaoriosHelper StatusManager.
Tracks and reports granular engine states, installation status, Zygisk readiness,
configuration generations, and reboot requirements.
"""

import json
import os
import stat
from enum import Enum
from pathlib import Path
from typing import Dict, Any


class UnifiedStatus(str, Enum):
    """Unified operational states defined in the KaoriosHelper specification."""
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


class StatusManager:
    """Aggregates and publishes operational state reports for Kaorios Toolbox."""

    def __init__(self, data_dir: Path):
        self.data_dir = Path(data_dir).resolve()
        self.status_file = self.data_dir / "status.json"

    def get_generation(self, engine_name: str, kind: str) -> int:
        """Read a non-negative per-engine generation, without trusting malformed files."""
        try:
            value = int((self.data_dir / f"{engine_name}.{kind}_gen").read_text().strip())
            return max(0, value)
        except (OSError, ValueError):
            return 0

    def get_runtime_ack_status(self, engine_name: str) -> UnifiedStatus:
        """Evaluate runtime evidence only; this is not device verification."""
        if engine_name not in ("hma", "pif", "tee", "copg"):
            return UnifiedStatus.PENDING_RELOAD
        stored = self.get_generation(engine_name, "stored")
        requested = self.get_generation(engine_name, "requested")
        runtime_dir = self.data_dir / "runtime"
        try:
            dir_fd = os.open(runtime_dir, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try:
                flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
                try:
                    fd = os.open(f"{engine_name}.ack.json", flags, dir_fd=dir_fd)
                except FileNotFoundError:
                    fd = os.open(f"{engine_name}.json", flags, dir_fd=dir_fd)
                try:
                    metadata = os.fstat(fd)
                    if (not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.geteuid() or
                            metadata.st_mode & 0o022 or not 0 < metadata.st_size <= 65536):
                        return UnifiedStatus.PENDING_RELOAD
                    with os.fdopen(fd, "rb", closefd=False) as stream:
                        content = stream.read(65537)
                    if len(content) > 65536:
                        return UnifiedStatus.PENDING_RELOAD
                    ack = json.loads(content)
                finally:
                    os.close(fd)
            finally:
                os.close(dir_fd)
        except (OSError, ValueError, UnicodeError):
            return UnifiedStatus.PENDING_RELOAD
        if not isinstance(ack, dict):
            return UnifiedStatus.PENDING_RELOAD
        integer_fields = ("schemaVersion", "protocolVersion", "pid", "uid",
                          "configGeneration", "initResult", "errorCode")
        if any(type(ack.get(field)) is not int for field in integer_fields):
            return UnifiedStatus.PENDING_RELOAD
        if (ack["schemaVersion"] != 1 or ack["protocolVersion"] != 1 or
                ack.get("engine") != engine_name or ack["pid"] <= 0 or
                not isinstance(ack.get("processName"), str) or not ack["processName"] or
                stored <= 0 or ack["configGeneration"] != stored or stored != requested):
            return UnifiedStatus.PENDING_RELOAD
        process = ack["processName"]
        if engine_name == "hma" and (process not in ("system_server", "android") or
                                      ack["uid"] not in (0, 1000)):
            return UnifiedStatus.PENDING_RELOAD
        if engine_name in ("pif", "tee"):
            processes = ("com.google.android.gms", "com.google.android.gms.unstable")
            if engine_name == "pif":
                processes += ("com.android.vending",)
            if process not in processes:
                return UnifiedStatus.PENDING_RELOAD
        event = ack.get("eventType")
        if event == "APPLY_FAILED" or ack["initResult"] != 0 or ack["errorCode"] != 0:
            return UnifiedStatus.APPLY_FAILED
        if event in ("HOOK_INITIALIZED", "HOOK_OBSERVED"):
            return UnifiedStatus.APPLIED
        return UnifiedStatus.PENDING_RELOAD

    def get_engine_status(self, engine_name: str) -> UnifiedStatus:
        """Evaluates actual engine status based on disk state, flags, and configuration."""
        disabled_marker = self.data_dir / f"{engine_name}.disabled"
        enabled_marker = self.data_dir / f"{engine_name}.enabled"

        if engine_name in ["tee", "copg"]:
            # TEE and COPG require explicit opt-in enablement
            if not enabled_marker.is_file():
                return UnifiedStatus.NOT_CONFIGURED
        else:
            # HMA and PIF are enabled by default unless explicitly disabled
            if disabled_marker.is_file():
                return UnifiedStatus.ENGINE_UNAVAILABLE

        # Check configuration existence and validity
        config_map = {
            "pif": "pif.json",
            "tee": "keybox.xml",
            "copg": "spoof.json",
            "hma": "hma.json",
        }
        cfg_name = config_map.get(engine_name)
        if cfg_name:
            cfg_file = self.data_dir / cfg_name
            if not cfg_file.is_file() or cfg_file.stat().st_size == 0:
                return UnifiedStatus.NOT_CONFIGURED

        return self.get_runtime_ack_status(engine_name)

    def generate_report(self, module_version: str = "v1.0.0", root_mode: str = "Root+Zygisk") -> Dict[str, Any]:
        """Builds a complete structured telemetry and readiness report."""
        engines = ["hma", "pif", "tee", "copg"]
        engine_reports = {}
        reboot_needed = False

        for eng in engines:
            st = self.get_engine_status(eng)
            if st == UnifiedStatus.REBOOT_REQUIRED:
                reboot_needed = True
            engine_reports[eng] = {
                "status": st.value,
                "enabled": (st in [UnifiedStatus.APPLIED, UnifiedStatus.PENDING_RELOAD, UnifiedStatus.APPLYING, UnifiedStatus.APPLY_FAILED]),
                "deviceVerified": False,
                "storedGeneration": self.get_generation(eng, "stored"),
                "requestedGeneration": self.get_generation(eng, "requested"),
                "appliedGeneration": (self.get_generation(eng, "stored")
                                      if st == UnifiedStatus.APPLIED else 0),
            }

        # Check generation
        gen_file = self.data_dir / "generation.txt"
        current_gen = 0
        if gen_file.is_file():
            try:
                current_gen = int(gen_file.read_text().strip())
            except ValueError:
                pass

        report = {
            "module": {
                "id": "kaorioshelper",
                "version": module_version,
                "rootMode": root_mode,
                "rebootRequired": reboot_needed,
                "storedGeneration": current_gen,
                "activeGeneration": 0,
            },
            "engines": engine_reports,
        }
        return report

    def publish_status(self, module_version: str = "v1.0.0") -> None:
        """Writes report atomically to status.json."""
        report = self.generate_report(module_version)
        tmp_status = self.data_dir / ".status.json.tmp"
        tmp_status.write_text(json.dumps(report, indent=2))
        tmp_status.replace(self.status_file)
