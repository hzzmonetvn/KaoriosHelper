#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import fcntl
import json
import multiprocessing
import os
import stat
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from core.config.config_manager import (
    ConfigManager,
    ConfigValidationError,
    ConfigSecurityError,
    CONFIG_PIF,
    CONFIG_KEYBOX,
    CONFIG_SPOOF,
    CONFIG_HMA,
)


def _locked_rollback(data_dir, connection):
    mgr = ConfigManager(data_dir)
    flock = fcntl.flock

    def observe_lock(fd, operation):
        if operation == fcntl.LOCK_EX:
            try:
                flock(fd, operation | fcntl.LOCK_NB)
            except BlockingIOError:
                connection.send("contended")
                flock(fd, operation)
        else:
            flock(fd, operation)

    connection.send("ready")
    connection.recv()
    with mock.patch("core.config.config_manager.fcntl.flock", side_effect=observe_lock):
        connection.send(mgr.rollback(CONFIG_HMA))
    connection.close()


class TestConfigManager(unittest.TestCase):
    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.data_dir = Path(self.tmp_dir.name)
        self.mgr = ConfigManager(self.data_dir)

    def tearDown(self):
        self.tmp_dir.cleanup()

    def test_initial_generation(self):
        self.assertEqual(self.mgr.get_generation(), 0)

    def test_atomic_write_pif(self):
        valid_pif = """{
            "MANUFACTURER": "Google",
            "MODEL": "Pixel 7 Pro",
            "FINGERPRINT": "google/cheetah/cheetah:14/...",
            "spoofBuild": 1,
            "spoofProps": 1,
            "spoofProvider": 0
        }"""
        ok, msg, gen = self.mgr.write_config_atomic(CONFIG_PIF, valid_pif)
        self.assertTrue(ok)
        self.assertEqual(gen, 1)
        self.assertEqual(self.mgr.get_generation(), 1)

        ok_read, content = self.mgr.read_config(CONFIG_PIF)
        self.assertTrue(ok_read)
        self.assertIn("Pixel 7 Pro", content)

    def test_malformed_json_rejection(self):
        bad_json = '{"MANUFACTURER": "Google", unclosed'
        ok, msg, gen = self.mgr.write_config_atomic(CONFIG_PIF, bad_json)
        self.assertFalse(ok)
        self.assertIn("Validation failure", msg)
        self.assertEqual(gen, 0)
        self.assertFalse((self.data_dir / CONFIG_PIF).exists())

    def test_keybox_xml_validation_and_redaction(self):
        valid_keybox = """<?xml version="1.0"?>
        <AndroidAttestation>
            <NumberOfKeyboxes>1</NumberOfKeyboxes>
            <Keybox DeviceID="test">
                <Key algorithm="ecdsa">
                    <PrivateKey format="pem">
                        MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg...
                    </PrivateKey>
                    <Certificate format="pem">
                        -----BEGIN CERTIFICATE-----
                        MIIC...
                        -----END CERTIFICATE-----
                    </Certificate>
                </Key>
            </Keybox>
        </AndroidAttestation>"""
        ok, msg, gen = self.mgr.write_config_atomic(CONFIG_KEYBOX, valid_keybox)
        self.assertTrue(ok)
        self.assertEqual(gen, 1)

    def test_empty_or_invalid_keybox_rejected(self):
        invalid_xml = "<InvalidRoot><NoKeysHere/></InvalidRoot>"
        ok, msg, gen = self.mgr.write_config_atomic(CONFIG_KEYBOX, invalid_xml)
        self.assertFalse(ok)
        self.assertIn("Validation failure", msg)

    def test_symlink_rejection(self):
        # Create a symlink pointing to an outside file
        outside_file = self.data_dir / "outside.txt"
        outside_file.write_text("secret")
        symlink_target = self.data_dir / CONFIG_PIF
        os.symlink(outside_file, symlink_target)

        valid_pif = '{"MANUFACTURER": "Google"}'
        ok, msg, _ = self.mgr.write_config_atomic(CONFIG_PIF, valid_pif)
        self.assertFalse(ok)
        self.assertIn("Symlinks are prohibited", msg)

    def test_path_traversal_rejection(self):
        with self.assertRaises(ConfigSecurityError):
            self.mgr._resolve_safe_path("../../../etc/passwd")

    def test_corrupt_active_does_not_replace_valid_backup(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 1}')[0])
        (self.data_dir / CONFIG_HMA).write_text('{broken')
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 2}')[0])
        self.assertTrue(self.mgr.rollback(CONFIG_HMA)[0])
        self.assertEqual((self.data_dir / CONFIG_HMA).read_text(), '{}')

    def test_invalid_backup_is_not_restored(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        (self.data_dir / f"{CONFIG_HMA}.last_known_good").write_text('{broken')
        self.assertFalse(self.mgr.rollback(CONFIG_HMA)[0])
        self.assertEqual((self.data_dir / CONFIG_HMA).read_text(), '{}')
        self.assertEqual(self.mgr.get_generation(), 1)

    def test_generation_publication_failure_is_recoverable_not_old_generation_success(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        original = self.mgr._atomic_write

        def fail_generation(directory, filename, content):
            if filename == "generation.txt":
                raise OSError("injected generation failure")
            original(directory, filename, content)

        with mock.patch.object(self.mgr, "_atomic_write", side_effect=fail_generation):
            ok, message, generation = self.mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 2}')
            self.assertFalse(self.mgr.read_config(CONFIG_HMA)[0])
        self.assertFalse(ok)
        self.assertIn("outcome unconfirmed", message)
        self.assertEqual(generation, -1)
        self.assertTrue((self.data_dir / ".config.transaction").is_file())
        recovered = ConfigManager(self.data_dir)
        self.assertEqual(recovered.get_generation(), 2)
        self.assertEqual(recovered.read_config(CONFIG_HMA), (True, '{"configVersion": 2}'))
        self.assertFalse((self.data_dir / ".config.transaction").exists())

    def test_failure_before_journal_publication_keeps_old_state(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        with mock.patch.object(self.mgr, "_atomic_write", side_effect=OSError("injected staging failure")):
            ok, message, generation = self.mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 2}')
        self.assertFalse(ok)
        self.assertIn("Write failure", message)
        self.assertEqual(generation, 1)
        self.assertEqual(self.mgr.read_config(CONFIG_HMA), (True, '{}'))
        self.assertFalse((self.data_dir / ".config.transaction").exists())

    def test_recovery_after_each_publication_boundary(self):
        class Interrupted(BaseException):
            pass

        for boundary in (".config.transaction", f"{CONFIG_HMA}.last_known_good",
                         CONFIG_HMA, "generation.txt"):
            with self.subTest(boundary=boundary), tempfile.TemporaryDirectory() as directory:
                mgr = ConfigManager(Path(directory))
                self.assertTrue(mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
                original = mgr._atomic_write

                def interrupt_after_publish(fd, filename, content):
                    original(fd, filename, content)
                    if filename == boundary:
                        raise Interrupted()

                with mock.patch.object(mgr, "_atomic_write", side_effect=interrupt_after_publish):
                    with self.assertRaises(Interrupted):
                        mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 2}')
                recovered = ConfigManager(Path(directory))
                self.assertEqual(recovered.get_generation(), 2)
                self.assertEqual(recovered.read_config(CONFIG_HMA), (True, '{"configVersion": 2}'))
                self.assertEqual((Path(directory) / f"{CONFIG_HMA}.last_known_good").read_text(), '{}')
                self.assertFalse((Path(directory) / ".config.transaction").exists())

    def test_journal_durability_failure_does_not_publish_payload(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        original = self.mgr._sync_directory

        def fail_journal_sync(directory):
            if (self.data_dir / ".config.transaction").exists():
                raise OSError("injected journal durability failure")
            original(directory)

        with mock.patch.object(self.mgr, "_sync_directory", side_effect=fail_journal_sync):
            ok, message, generation = self.mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 2}')
            self.assertFalse(ok)
            self.assertIn("outcome unconfirmed", message)
            self.assertEqual(generation, -1)
            self.assertFalse(self.mgr.read_config(CONFIG_HMA)[0])
            self.assertEqual((self.data_dir / CONFIG_HMA).read_text(), '{}')
            self.assertEqual((self.data_dir / "generation.txt").read_text(), '1\n')
        self.assertEqual(self.mgr.get_generation(), 2)
        self.assertEqual(self.mgr.read_config(CONFIG_HMA), (True, '{"configVersion": 2}'))

    def test_recovery_is_idempotent_after_journal_cleanup_sync_failure(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        original = self.mgr._sync_directory

        def fail_cleanup_sync(directory):
            original(directory)
            if not (self.data_dir / ".config.transaction").exists():
                raise OSError("injected cleanup sync failure")

        with mock.patch.object(self.mgr, "_sync_directory", side_effect=fail_cleanup_sync):
            ok, message, generation = self.mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 2}')
        self.assertFalse(ok)
        self.assertIn("outcome unconfirmed", message)
        self.assertEqual(generation, -1)
        self.assertEqual(self.mgr.get_generation(), 2)
        self.assertEqual(self.mgr.read_config(CONFIG_HMA), (True, '{"configVersion": 2}'))

    def test_rollback_generation_failure_recovers_same_rollback(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 2}')[0])
        original = self.mgr._atomic_write

        def fail_generation(directory, filename, content):
            if filename == "generation.txt":
                raise OSError("injected generation failure")
            original(directory, filename, content)

        with mock.patch.object(self.mgr, "_atomic_write", side_effect=fail_generation):
            ok, message = self.mgr.rollback(CONFIG_HMA)
        self.assertFalse(ok)
        self.assertIn("outcome unconfirmed", message)
        self.assertEqual(self.mgr.get_generation(), 3)
        self.assertEqual(self.mgr.read_config(CONFIG_HMA), (True, '{}'))

    def test_internal_symlinks_and_hardlinks_are_rejected(self):
        for leaf in (".config.lock", "generation.txt", ".config.transaction",
                     CONFIG_HMA, f"{CONFIG_HMA}.last_known_good"):
            for hardlink in (False, True):
                with self.subTest(leaf=leaf, hardlink=hardlink), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    mgr = ConfigManager(root)
                    outside = root / "outside"
                    outside.write_text("1" if leaf == "generation.txt" else "{}")
                    target = root / leaf
                    target.unlink(missing_ok=True)
                    if hardlink:
                        os.link(outside, target)
                    else:
                        target.symlink_to(outside)
                    self.assertFalse(mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
                    self.assertEqual(outside.read_text(), "1" if leaf == "generation.txt" else "{}")

    def test_recovery_rejects_malformed_or_wrong_generation_journal(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        journal = self.data_dir / ".config.transaction"
        invalid = ["{broken", json.dumps(dict(version=1, filename="../outside", content="{}",
                                             generation=2, backup=None)),
                   json.dumps(dict(version=1, filename=CONFIG_HMA, content="{}",
                                   generation=9, backup=None))]
        for payload in invalid:
            with self.subTest(payload=payload):
                journal.write_text(payload)
                self.assertFalse(self.mgr.read_config(CONFIG_HMA)[0])
                self.assertFalse(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
                self.assertEqual((self.data_dir / CONFIG_HMA).read_text(), '{}')
                self.assertEqual((self.data_dir / "generation.txt").read_text(), '1\n')
        journal.unlink()

    def test_backup_link_is_not_followed_during_rollback(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        outside = self.data_dir / "outside"
        outside.write_text('{"configVersion": 9}')
        backup = self.data_dir / f"{CONFIG_HMA}.last_known_good"
        backup.symlink_to(outside)
        self.assertFalse(self.mgr.rollback(CONFIG_HMA)[0])
        self.assertEqual(self.mgr.read_config(CONFIG_HMA), (True, '{}'))
        self.assertEqual(self.mgr.get_generation(), 1)

    def test_private_staging_is_unique_and_cleaned_on_failure(self):
        original = os.replace
        observed = []

        def inspect_replace(source, destination, **kwargs):
            info = os.stat(source, dir_fd=kwargs["src_dir_fd"], follow_symlinks=False)
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
            self.assertNotIn(source, observed)
            observed.append(source)
            if destination == CONFIG_HMA:
                raise OSError("injected replacement failure")
            original(source, destination, **kwargs)

        with mock.patch("core.config.config_manager.os.replace", side_effect=inspect_replace):
            self.assertEqual(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[2], -1)
        self.assertEqual(list(self.data_dir.glob(".config-stage.*")), [])
        self.assertGreaterEqual(len(observed), 2)
        self.assertEqual(stat.S_IMODE((self.data_dir / ".config.transaction").stat().st_mode), 0o600)
        self.assertEqual(self.mgr.get_generation(), 1)
        self.assertEqual(stat.S_IMODE((self.data_dir / CONFIG_HMA).stat().st_mode), 0o600)

    def test_rollback_replaces_complete_file_without_truncating_live_target(self):
        previous = '{"configVersion": 1}'
        active = '{"configVersion": 2}'
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, previous)[0])
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, active)[0])
        original = os.replace
        observed = []

        def inspect_replace(source, destination, **kwargs):
            if destination == CONFIG_HMA:
                observed.append(destination)
                self.assertEqual((self.data_dir / CONFIG_HMA).read_text(), active)
                fd = os.open(source, os.O_RDONLY, dir_fd=kwargs["src_dir_fd"])
                with os.fdopen(fd) as staged:
                    self.assertEqual(staged.read(), previous)
            original(source, destination, **kwargs)

        with mock.patch("core.config.config_manager.os.replace", side_effect=inspect_replace):
            self.assertTrue(self.mgr.rollback(CONFIG_HMA)[0])
        self.assertEqual(observed, [CONFIG_HMA])
        self.assertEqual(self.mgr.read_config(CONFIG_HMA), (True, previous))

    def test_rollback_waits_for_shared_writer_lock(self):
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{}')[0])
        self.assertTrue(self.mgr.write_config_atomic(CONFIG_HMA, '{"configVersion": 2}')[0])
        context = multiprocessing.get_context("fork")
        parent, child = context.Pipe()
        process = context.Process(target=_locked_rollback, args=(self.data_dir, child))
        process.start()
        child.close()
        try:
            self.assertTrue(parent.poll(10), "rollback worker did not initialize")
            self.assertEqual(parent.recv(), "ready")
            with open(self.mgr.lock_file, "r+") as held:
                fcntl.flock(held, fcntl.LOCK_EX)
                parent.send("rollback")
                self.assertTrue(parent.poll(10), "rollback did not reach the held writer lock")
                self.assertEqual(parent.recv(), "contended")
                self.assertFalse(parent.poll(), "rollback returned while the writer lock was held")
                self.assertEqual((self.data_dir / CONFIG_HMA).read_text(), '{"configVersion": 2}')
                fcntl.flock(held, fcntl.LOCK_UN)
            self.assertTrue(parent.poll(10), "rollback did not finish after lock release")
            self.assertTrue(parent.recv()[0])
            process.join(10)
            self.assertEqual(process.exitcode, 0)
        finally:
            parent.close()
            if process.is_alive():
                process.terminate()
            process.join(10)
        self.assertEqual(self.mgr.read_config(CONFIG_HMA), (True, '{}'))
        self.assertEqual(self.mgr.get_generation(), 3)

    def test_rollback_to_last_known_good(self):
        # First write good config
        good_pif = '{"MANUFACTURER": "Google", "MODEL": "Pixel 7"}'
        self.mgr.write_config_atomic(CONFIG_PIF, good_pif)
        self.assertEqual(self.mgr.get_generation(), 1)

        # Second write updated good config
        good_pif2 = '{"MANUFACTURER": "Google", "MODEL": "Pixel 8"}'
        self.mgr.write_config_atomic(CONFIG_PIF, good_pif2)
        self.assertEqual(self.mgr.get_generation(), 2)

        # Rollback
        ok_rb, msg_rb = self.mgr.rollback(CONFIG_PIF)
        self.assertTrue(ok_rb)
        self.assertEqual(self.mgr.get_generation(), 3)

        _, read_back = self.mgr.read_config(CONFIG_PIF)
        self.assertIn("Pixel 7", read_back)


if __name__ == "__main__":
    unittest.main()
