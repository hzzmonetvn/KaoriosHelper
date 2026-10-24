#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
KaoriosHelper ConfigManager.
Implements safe, atomic configuration storage with versioned schemas,
file locking, symlink/traversal rejection, generation tracking, and rollback.
"""

import os
import sys
import json
import fcntl
import shutil
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Dict, Any, Tuple, Optional

# Supported configuration filenames
CONFIG_PIF = "pif.json"
CONFIG_KEYBOX = "keybox.xml"
CONFIG_SPOOF = "spoof.json"
CONFIG_HMA = "hma.json"

ALLOWED_CONFIG_FILES = {CONFIG_PIF, CONFIG_KEYBOX, CONFIG_SPOOF, CONFIG_HMA}


class ConfigValidationError(Exception):
    """Raised when configuration validation fails."""
    pass


class ConfigSecurityError(Exception):
    """Raised when security boundaries (symlinks, path traversal) are violated."""
    pass


class ConfigManager:
    """Manages reading, validating, and atomically persisting module configuration."""

    def __init__(self, data_dir: Path):
        self.data_dir = Path(data_dir).resolve()
        self.lock_file = self.data_dir / ".config.lock"
        self.gen_file = self.data_dir / "generation.txt"
        self._ensure_directories()

    def _ensure_directories(self) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)
        try:
            os.chmod(self.data_dir, 0o700)
        except OSError:
            pass

    def get_generation(self) -> int:
        """Returns the current configuration generation counter."""
        if not self.gen_file.is_file():
            return 0
        try:
            return int(self.gen_file.read_text().strip())
        except (ValueError, OSError):
            return 0

    def _increment_generation(self) -> int:
        current = self.get_generation()
        new_gen = current + 1
        tmp_gen = self.data_dir / f".gen.tmp.{os.getpid()}"
        tmp_gen.write_text(f"{new_gen}\n")
        tmp_gen.replace(self.gen_file)
        return new_gen

    def _resolve_safe_path(self, filename: str) -> Path:
        """Ensures the filename is strictly in the allowed set and prevents directory traversal."""
        if filename not in ALLOWED_CONFIG_FILES:
            raise ConfigSecurityError(f"Unauthorized configuration file: {filename}")

        direct_path = self.data_dir / filename
        if direct_path.is_symlink():
            raise ConfigSecurityError(f"Symlinks are prohibited: {direct_path}")

        target = direct_path.resolve()
        if not str(target).startswith(str(self.data_dir)):
            raise ConfigSecurityError(f"Path traversal detected: {filename}")

        if target.is_symlink():
            raise ConfigSecurityError(f"Symlinks are prohibited: {target}")

        return target

    def validate_pif_config(self, content: str) -> Dict[str, Any]:
        """Validates Play Integrity Fix JSON payload."""
        try:
            data = json.loads(content)
        except json.JSONDecodeError as e:
            raise ConfigValidationError(f"Malformed PIF JSON: {e}")

        if not isinstance(data, dict):
            raise ConfigValidationError("PIF config must be a JSON object")

        # Verify required property keys
        for key in ["MANUFACTURER", "MODEL", "FINGERPRINT", "BRAND", "PRODUCT", "DEVICE"]:
            if key in data and not isinstance(data[key], str):
                raise ConfigValidationError(f"PIF field {key} must be a string")

        # Verify advanced flags if present
        for flag in ["spoofBuild", "spoofProps", "spoofProvider", "spoofSignature"]:
            if flag in data and not isinstance(data[flag], (int, bool)):
                raise ConfigValidationError(f"PIF flag {flag} must be an integer or boolean")

        return data

    def validate_keybox_xml(self, content: str) -> str:
        """Validates Keybox XML structure without logging or exposing private key material."""
        if not content.strip():
            raise ConfigValidationError("Keybox XML content is empty")

        try:
            root = ET.fromstring(content)
        except ET.ParseError as e:
            raise ConfigValidationError(f"Malformed Keybox XML: {e}")

        # Keybox XML root should typically be <AndroidAttestation> or <Keybox>
        tag = root.tag.lower()
        if "keybox" not in tag and "attestation" not in tag:
            raise ConfigValidationError(f"Unrecognized Keybox root element: {root.tag}")

        # Verify presence of at least one key algorithm container (ecdsa or rsa)
        has_key = False
        for child in root.iter():
            child_tag = child.tag.lower()
            if "privatekey" in child_tag or "certificate" in child_tag or "key" in child_tag:
                has_key = True
                break

        if not has_key:
            raise ConfigValidationError("Keybox XML contains no identifiable Key or Certificate elements")

        return content

    def validate_spoof_config(self, content: str) -> Dict[str, Any]:
        """Validates COPG game & device spoofing JSON payload."""
        try:
            data = json.loads(content)
        except json.JSONDecodeError as e:
            raise ConfigValidationError(f"Malformed Spoof JSON: {e}")

        if not isinstance(data, dict):
            raise ConfigValidationError("Spoof config must be a JSON object")

        # Check cpu_spoof section if present
        if "cpu_spoof" in data:
            cpu = data["cpu_spoof"]
            if not isinstance(cpu, dict):
                raise ConfigValidationError("cpu_spoof must be an object")
            for list_key in ["blacklist", "cpu_only_packages"]:
                if list_key in cpu and not isinstance(cpu[list_key], list):
                    raise ConfigValidationError(f"cpu_spoof.{list_key} must be a list")

        # Check device profiles
        for key, value in data.items():
            if key.startswith("PACKAGES_") and key.endswith("_DEVICE"):
                if not isinstance(value, dict):
                    raise ConfigValidationError(f"Device profile {key} must be an object")
            elif key.startswith("PACKAGES_"):
                if not isinstance(value, list):
                    raise ConfigValidationError(f"Package list {key} must be an array")

        return data

    def validate_hma_config(self, content: str) -> Dict[str, Any]:
        """Validates HMA-OSS package hiding JSON payload."""
        try:
            data = json.loads(content)
        except json.JSONDecodeError as e:
            raise ConfigValidationError(f"Malformed HMA JSON: {e}")

        if not isinstance(data, dict):
            raise ConfigValidationError("HMA config must be a JSON object")

        if "configVersion" in data and not isinstance(data["configVersion"], int):
            raise ConfigValidationError("configVersion must be an integer")

        for map_key in ["templates", "settingsTemplates", "scope"]:
            if map_key in data and not isinstance(data[map_key], dict):
                raise ConfigValidationError(f"HMA field {map_key} must be a dictionary")

        return data

    def validate_payload(self, filename: str, content: str) -> None:
        """Validates content according to the target file schema."""
        if filename == CONFIG_PIF:
            self.validate_pif_config(content)
        elif filename == CONFIG_KEYBOX:
            self.validate_keybox_xml(content)
        elif filename == CONFIG_SPOOF:
            self.validate_spoof_config(content)
        elif filename == CONFIG_HMA:
            self.validate_hma_config(content)

    def read_config(self, filename: str) -> Tuple[bool, str]:
        """Safely reads configuration file content with validation."""
        try:
            target = self._resolve_safe_path(filename)
            if not target.is_file():
                return False, f"File not found: {filename}"
            return True, target.read_text(encoding="utf-8")
        except Exception as e:
            return False, str(e)

    def write_config_atomic(self, filename: str, content: str) -> Tuple[bool, str, int]:
        """
        Atomically commits configuration data.
        Returns (success: bool, message: str, generation: int).
        """
        try:
            target = self._resolve_safe_path(filename)
            self.validate_payload(filename, content)

            # Acquire exclusive lock
            lock_fd = open(self.lock_file, "w")
            try:
                fcntl.flock(lock_fd.fileno(), fcntl.LOCK_EX)

                # Create backup of existing file if valid
                backup_path = self.data_dir / f"{filename}.last_known_good"
                if target.is_file():
                    shutil.copy2(target, backup_path)

                # Write to temp file in the same directory (guarantee same filesystem for rename)
                tmp_file = self.data_dir / f".{filename}.tmp.{os.getpid()}"
                with open(tmp_file, "w", encoding="utf-8") as f:
                    f.write(content)
                    f.flush()
                    os.fsync(f.fileno())

                # Restrict permissions: 0600 (owner read/write only)
                os.chmod(tmp_file, 0o600)

                # Atomic replacement
                tmp_file.replace(target)

                # Record generation
                new_gen = self._increment_generation()
                return True, f"Successfully committed {filename}", new_gen

            finally:
                fcntl.flock(lock_fd.fileno(), fcntl.LOCK_UN)
                lock_fd.close()

        except (ConfigValidationError, ConfigSecurityError) as e:
            return False, f"Validation failure: {e}", self.get_generation()
        except Exception as e:
            return False, f"Write failure: {e}", self.get_generation()

    def rollback(self, filename: str) -> Tuple[bool, str]:
        """Restores last-known-good configuration if available."""
        try:
            target = self._resolve_safe_path(filename)
            backup_path = self.data_dir / f"{filename}.last_known_good"
            if not backup_path.is_file():
                return False, f"No backup found for {filename}"

            shutil.copy2(backup_path, target)
            new_gen = self._increment_generation()
            return True, f"Rolled back {filename} to last known good (gen {new_gen})"
        except Exception as e:
            return False, f"Rollback failed: {e}"
