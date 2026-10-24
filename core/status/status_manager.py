#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
KaoriosHelper StatusManager.
Tracks and reports granular engine states, installation status, Zygisk readiness,
configuration generations, and reboot requirements.
"""

import json
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

        # Check reload pending flags
        reload_marker = self.data_dir / f"{engine_name}.reload_pending"
        if reload_marker.is_file():
            return UnifiedStatus.PENDING_RELOAD

        return UnifiedStatus.APPLIED

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
                "enabled": (st in [UnifiedStatus.APPLIED, UnifiedStatus.PENDING_RELOAD, UnifiedStatus.APPLYING]),
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
                "activeGeneration": current_gen,
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
