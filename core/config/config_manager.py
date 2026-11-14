#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""
KaoriosHelper ConfigManager.
Implements safe, atomic configuration storage with versioned schemas,
file locking, symlink/traversal rejection, generation tracking, and rollback.
"""

import os
import json
import fcntl
import stat
import secrets
import xml.etree.ElementTree as ET
from contextlib import contextmanager
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


class ConfigTransactionError(Exception):
    """A published transaction needs recovery before its outcome is known."""
    pass


class ConfigManager:
    """Serializes management reads/writes and recovers interrupted publications.

    Raw file readers do not share the lock and may see an in-progress transaction.
    The legacy global generation is separate from native per-engine generations.
    """

    def __init__(self, data_dir: Path):
        self.data_dir = Path(data_dir).resolve()
        self.lock_file = self.data_dir / ".config.lock"
        self.gen_file = self.data_dir / "generation.txt"
        self._ensure_directories()
        with self._locked():
            pass

    def _ensure_directories(self) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)
        os.chmod(self.data_dir, 0o700)

    @staticmethod
    def _check_stat(info) -> None:
        if stat.S_ISLNK(info.st_mode):
            raise ConfigSecurityError("Symlinks are prohibited")
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1
                or info.st_uid != os.geteuid() or info.st_mode & 0o022):
            raise ConfigSecurityError("Untrusted configuration file")

    def _check_leaf(self, directory: int, filename: str) -> None:
        try:
            info = os.stat(filename, dir_fd=directory, follow_symlinks=False)
        except FileNotFoundError:
            return
        self._check_stat(info)

    def _read_leaf(self, directory: int, filename: str) -> Optional[str]:
        self._check_leaf(directory, filename)
        try:
            fd = os.open(filename, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
                         | os.O_CLOEXEC, dir_fd=directory)
        except FileNotFoundError:
            return None
        with os.fdopen(fd, "r", encoding="utf-8") as stream:
            self._check_stat(os.fstat(stream.fileno()))
            return stream.read()

    def _sync_directory(self, directory: int) -> None:
        os.fsync(directory)

    def _atomic_write(self, directory: int, filename: str, content: str) -> None:
        self._check_leaf(directory, filename)
        temporary = f".config-stage.{secrets.token_hex(16)}"
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                     | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=directory)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, filename, src_dir_fd=directory, dst_dir_fd=directory)
            self._sync_directory(directory)
        finally:
            try:
                os.unlink(temporary, dir_fd=directory)
            except FileNotFoundError:
                pass

    @contextmanager
    def _locked(self):
        directory = os.open(self.data_dir, os.O_RDONLY | os.O_DIRECTORY
                            | os.O_NOFOLLOW | os.O_CLOEXEC)
        lock = None
        try:
            info = os.fstat(directory)
            if info.st_uid != os.geteuid() or info.st_mode & 0o022:
                raise ConfigSecurityError("Untrusted configuration directory")
            self._check_leaf(directory, ".config.lock")
            lock = os.open(".config.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW
                           | os.O_NONBLOCK | os.O_CLOEXEC, 0o600, dir_fd=directory)
            self._check_stat(os.fstat(lock))
            fcntl.flock(lock, fcntl.LOCK_EX)
            self._recover(directory)
            yield directory
        finally:
            if lock is not None:
                os.close(lock)
            os.close(directory)

    def _generation(self, directory: int) -> int:
        text = self._read_leaf(directory, "generation.txt")
        if text is None:
            return 0
        value = int(text.strip())
        if value < 0:
            raise ConfigValidationError("Invalid configuration generation")
        return value

    def get_generation(self) -> int:
        """Returns the recovered legacy global generation, never a pending value."""
        with self._locked() as directory:
            return self._generation(directory)

    def _resolve_safe_path(self, filename: str) -> Path:
        """Only supported single leaf names are accepted."""
        if filename not in ALLOWED_CONFIG_FILES:
            raise ConfigSecurityError(f"Unauthorized configuration file: {filename}")
        target = self.data_dir / filename
        try:
            self._check_stat(target.lstat())
        except FileNotFoundError:
            pass
        return target

    def _recover(self, directory: int) -> None:
        text = self._read_leaf(directory, ".config.transaction")
        if text is None:
            return
        try:
            transaction = json.loads(text)
            filename = transaction["filename"]
            content = transaction["content"]
            generation = transaction["generation"]
            backup = transaction["backup"]
            if (transaction["version"] != 1 or filename not in ALLOWED_CONFIG_FILES
                    or type(generation) is not int or generation < 1
                    or not isinstance(content, str)
                    or (backup is not None and not isinstance(backup, str))):
                raise ConfigValidationError("Invalid configuration transaction")
            self.validate_payload(filename, content)
            if backup is not None:
                self.validate_payload(filename, backup)
            for leaf in (filename, f"{filename}.last_known_good", "generation.txt"):
                self._check_leaf(directory, leaf)
            if self._generation(directory) not in (generation - 1, generation):
                raise ConfigValidationError("Configuration transaction generation mismatch")
            self._sync_directory(directory)
            if backup is not None:
                self._atomic_write(directory, f"{filename}.last_known_good", backup)
            self._atomic_write(directory, filename, content)
            self._atomic_write(directory, "generation.txt", f"{generation}\n")
            os.unlink(".config.transaction", dir_fd=directory)
            self._sync_directory(directory)
        except Exception as error:
            raise ConfigTransactionError("Configuration transaction pending recovery") from error

    def _commit(self, directory: int, filename: str, content: str,
                backup: Optional[str]) -> int:
        generation = self._generation(directory) + 1
        transaction = json.dumps(dict(version=1, filename=filename, content=content,
                                      generation=generation, backup=backup))
        try:
            self._atomic_write(directory, ".config.transaction", transaction)
            self._recover(directory)
        except Exception as error:
            try:
                pending = self._read_leaf(directory, ".config.transaction") is not None
            except Exception:
                pending = True
            if pending:
                raise ConfigTransactionError("Configuration transaction pending recovery") from error
            raise
        return generation

    def _failure_generation(self) -> int:
        try:
            return self.get_generation()
        except Exception:
            return -1

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
        """Safely reads a configuration after recovering pending publication."""
        try:
            self._resolve_safe_path(filename)
            with self._locked() as directory:
                content = self._read_leaf(directory, filename)
                if content is None:
                    return False, f"File not found: {filename}"
                return True, content
        except Exception as e:
            return False, str(e)

    def write_config_atomic(self, filename: str, content: str) -> Tuple[bool, str, int]:
        """Commits configuration with recoverable generation tracking.

        Returns (success, message, generation). Generation -1 means the published
        transaction's outcome is unconfirmed; a management read recovers it.
        """
        try:
            self._resolve_safe_path(filename)
            self.validate_payload(filename, content)
            with self._locked() as directory:
                self._check_leaf(directory, f"{filename}.last_known_good")
                previous = self._read_leaf(directory, filename)
                if previous is not None:
                    try:
                        self.validate_payload(filename, previous)
                    except ConfigValidationError:
                        previous = None
                new_gen = self._commit(directory, filename, content, previous)
            return True, f"Successfully committed {filename}", new_gen
        except ConfigTransactionError:
            return False, "Write outcome unconfirmed: configuration transaction pending recovery", -1
        except (ConfigValidationError, ConfigSecurityError) as e:
            return False, f"Validation failure: {e}", self._failure_generation()
        except Exception as e:
            return False, f"Write failure: {e}", self._failure_generation()

    def rollback(self, filename: str) -> Tuple[bool, str]:
        """Restores a validated last-known-good payload under the writer lock."""
        try:
            self._resolve_safe_path(filename)
            with self._locked() as directory:
                content = self._read_leaf(directory, f"{filename}.last_known_good")
                if content is None:
                    return False, f"No backup found for {filename}"
                self.validate_payload(filename, content)
                new_gen = self._commit(directory, filename, content, None)
            return True, f"Rolled back {filename} to last known good (gen {new_gen})"
        except ConfigTransactionError:
            return False, "Rollback outcome unconfirmed: configuration transaction pending recovery"
        except Exception as e:
            return False, f"Rollback failed: {e}"
