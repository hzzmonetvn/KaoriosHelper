#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
KaoriosHelper RuntimeManager.
Orchestrates engine lifecycle, independent enable/disable toggles,
reload requests, and health evaluation.
"""

from pathlib import Path
from typing import Dict, Any, Tuple
from core.status.status_manager import StatusManager, UnifiedStatus
from core.config.config_manager import ConfigManager
from core.registry.hook_registry import HookRegistry, HookConflictError
from core.logging.diagnostic_logger import DiagnosticLogger


class RuntimeManager:
    """Central lifecycle coordinator for KaoriosHelper engines."""

    VALID_ENGINES = {"hma", "pif", "tee", "copg"}

    def __init__(self, data_dir: Path):
        self.data_dir = Path(data_dir).resolve()
        self.config_mgr = ConfigManager(self.data_dir)
        self.status_mgr = StatusManager(self.data_dir)
        self.hook_registry = HookRegistry()
        self.logger = DiagnosticLogger(self.data_dir / "logs")

    def is_engine_enabled(self, engine: str) -> bool:
        """Checks if an engine is enabled."""
        if engine not in self.VALID_ENGINES:
            return False
        if engine in ["tee", "copg"]:
            return (self.data_dir / f"{engine}.enabled").is_file()
        else:
            return not (self.data_dir / f"{engine}.disabled").is_file()

    def enable_engine(self, engine: str) -> Tuple[bool, str]:
        """Enables an engine and marks pending reload."""
        if engine not in self.VALID_ENGINES:
            return False, f"Unknown engine: {engine}"

        if engine in ["tee", "copg"]:
            enabled_marker = self.data_dir / f"{engine}.enabled"
            enabled_marker.touch()
        else:
            disabled_marker = self.data_dir / f"{engine}.disabled"
            if disabled_marker.is_file():
                disabled_marker.unlink()

        # Check coexistence policy
        if engine == "tee":
            # Check PIF config
            ok, pif_content = self.config_mgr.read_config("pif.json")
            if ok:
                try:
                    pif_data = self.config_mgr.validate_pif_config(pif_content)
                    spoof_prov = pif_data.get("spoofProvider", 0)
                    valid, err = self.hook_registry.validate_pif_tee_coexistence(spoof_prov, True)
                    if not valid:
                        self.logger.warn("RuntimeManager", err or "")
                except Exception:
                    pass

        reload_marker = self.data_dir / f"{engine}.reload_pending"
        reload_marker.touch()

        self.logger.info("RuntimeManager", f"Engine '{engine}' enabled")
        self.status_mgr.publish_status()
        return True, f"Engine '{engine}' enabled (reload pending)"

    def disable_engine(self, engine: str) -> Tuple[bool, str]:
        """Disables an engine and marks pending reload."""
        if engine not in self.VALID_ENGINES:
            return False, f"Unknown engine: {engine}"

        if engine in ["tee", "copg"]:
            enabled_marker = self.data_dir / f"{engine}.enabled"
            if enabled_marker.is_file():
                enabled_marker.unlink()
        else:
            disabled_marker = self.data_dir / f"{engine}.disabled"
            disabled_marker.touch()

        reload_marker = self.data_dir / f"{engine}.reload_pending"
        reload_marker.touch()

        self.logger.info("RuntimeManager", f"Engine '{engine}' disabled")
        self.status_mgr.publish_status()
        return True, f"Engine '{engine}' disabled (reload pending)"

    def trigger_reload(self, engine: str) -> Tuple[bool, str]:
        """Clears pending reload marker once runtime engine acknowledges refresh."""
        if engine not in self.VALID_ENGINES:
            return False, f"Unknown engine: {engine}"
        reload_marker = self.data_dir / f"{engine}.reload_pending"
        if reload_marker.is_file():
            reload_marker.unlink()
        self.status_mgr.publish_status()
        return True, f"Engine '{engine}' reloaded successfully"
