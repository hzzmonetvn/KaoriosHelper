#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""Compatibility lifecycle API delegated to native configuration transactions."""

from pathlib import Path
from typing import Tuple
from core.status.status_manager import NativeControlError, StatusManager


class RuntimeManager:
    """Requests desired state; successful commands do not establish applied state."""

    VALID_ENGINES = {"hma", "pif", "tee", "copg"}

    def __init__(self, data_dir: Path, control_path: Path = None):
        self.data_dir = Path(data_dir).absolute()
        self.status_mgr = StatusManager(self.data_dir, control_path)

    def is_engine_enabled(self, engine: str) -> bool:
        if engine not in self.VALID_ENGINES:
            return False
        report = self.status_mgr.generate_report()
        return report["module"]["active"] and report["engines"][engine]["enabled"]

    def _request(self, action: str, engine: str) -> Tuple[bool, str]:
        if engine not in self.VALID_ENGINES:
            return False, f"Unknown engine: {engine}"
        try:
            self.status_mgr._check_control()
            self.status_mgr._run_control(action, engine)
        except NativeControlError:
            return False, "Native control request failed; inspect status and retry explicitly"
        return True, f"Engine '{engine}' {action} request accepted; runtime application pending"

    def enable_engine(self, engine: str) -> Tuple[bool, str]:
        return self._request("enable", engine)

    def disable_engine(self, engine: str) -> Tuple[bool, str]:
        return self._request("disable", engine)

    def trigger_reload(self, engine: str) -> Tuple[bool, str]:
        """Accept a fresh native request, never clear pending based on an old ACK."""
        return self._request("reload", engine)
