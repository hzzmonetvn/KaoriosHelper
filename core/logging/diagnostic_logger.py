#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
KaoriosHelper DiagnosticLogger.
Provides structured diagnostic logging with automatic redaction of sensitive keybox,
certificate, and private key data.
"""

import os
import re
import time
from pathlib import Path
from typing import Optional


class DiagnosticLogger:
    """Structured, privacy-aware logger for KaoriosHelper."""

    # Patterns matching sensitive cryptographic keys and credentials
    REDACTION_PATTERNS = [
        (re.compile(r"<PrivateKey>.*?</PrivateKey>", re.IGNORECASE | re.DOTALL), "<PrivateKey>[REDACTED]</PrivateKey>"),
        (re.compile(r"<Certificate>.*?</Certificate>", re.IGNORECASE | re.DOTALL), "<Certificate>[REDACTED]</Certificate>"),
        (re.compile(r"-----BEGIN (?:RSA |EC )?PRIVATE KEY-----.*?-----END (?:RSA |EC )?PRIVATE KEY-----", re.DOTALL), "[PRIVATE KEY REDACTED]"),
        (re.compile(r"(?i)\b(password|secret|token)\s*[:=]\s*\S+"), r"\1=[REDACTED]"),
    ]

    def __init__(self, log_dir: Path, max_size_bytes: int = 512 * 1024):
        self.log_dir = Path(log_dir).resolve()
        self.log_file = self.log_dir / "runtime.log"
        self.old_log_file = self.log_dir / "runtime.log.old"
        self.max_size = max_size_bytes
        self.log_dir.mkdir(parents=True, exist_ok=True)

    def redact(self, text: str) -> str:
        """Strips sensitive cryptographic material from message strings."""
        redacted = text
        for pattern, replacement in self.REDACTION_PATTERNS:
            redacted = pattern.sub(replacement, redacted)
        return redacted

    def _rotate_if_needed(self) -> None:
        if self.log_file.is_file() and self.log_file.stat().st_size >= self.max_size:
            if self.old_log_file.is_file():
                self.old_log_file.unlink()
            self.log_file.rename(self.old_log_file)

    def log(self, level: str, tag: str, message: str) -> None:
        """Writes a redacted structured log entry."""
        self._rotate_if_needed()
        clean_msg = self.redact(message)
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        entry = f"[{timestamp}] [{level.upper()}] [{tag}]: {clean_msg}\n"
        with open(self.log_file, "a", encoding="utf-8") as f:
            f.write(entry)

    def info(self, tag: str, message: str) -> None:
        self.log("INFO", tag, message)

    def warn(self, tag: str, message: str) -> None:
        self.log("WARN", tag, message)

    def error(self, tag: str, message: str) -> None:
        self.log("ERROR", tag, message)
