#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

import tempfile
import unittest
from pathlib import Path

from core.logging.diagnostic_logger import DiagnosticLogger


class TestDiagnosticLogger(unittest.TestCase):
    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.log_dir = Path(self.tmp_dir.name)
        self.logger = DiagnosticLogger(self.log_dir, max_size_bytes=1024)

    def tearDown(self):
        self.tmp_dir.cleanup()

    def test_xml_keybox_redaction(self):
        raw_msg = "Received keybox payload: <PrivateKey>SECRET_PRIVATE_KEY_BYTES</PrivateKey> in memory"
        redacted = self.logger.redact(raw_msg)
        self.assertNotIn("SECRET_PRIVATE_KEY_BYTES", redacted)
        self.assertIn("<PrivateKey>[REDACTED]</PrivateKey>", redacted)

        raw_cert = "<Certificate>SECRET_CERTIFICATE_DATA</Certificate>"
        redacted_cert = self.logger.redact(raw_cert)
        self.assertNotIn("SECRET_CERTIFICATE_DATA", redacted_cert)
        self.assertIn("<Certificate>[REDACTED]</Certificate>", redacted_cert)

    def test_pem_private_key_redaction(self):
        pem_key = """Here is the key:
-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIIz...SUPER_SECRET_KEY...
-----END EC PRIVATE KEY-----
Done."""
        redacted = self.logger.redact(pem_key)
        self.assertNotIn("SUPER_SECRET_KEY", redacted)
        self.assertIn("[PRIVATE KEY REDACTED]", redacted)

    def test_log_file_writing_and_redaction(self):
        self.logger.info("TestTag", "User provided credentials: password=MySecretPass123!")
        log_file = self.log_dir / "runtime.log"
        self.assertTrue(log_file.is_file())

        content = log_file.read_text()
        self.assertIn("[INFO] [TestTag]:", content)
        self.assertNotIn("MySecretPass123!", content)
        self.assertIn("password=[REDACTED]", content)

    def test_log_rotation(self):
        # Write enough logs to trigger rotation (> 1024 bytes)
        for i in range(50):
            self.logger.info("Flood", f"This is log line {i} to fill the log file buffer rapidly.")

        log_file = self.log_dir / "runtime.log"
        old_log = self.log_dir / "runtime.log.old"

        self.assertTrue(log_file.is_file())
        self.assertTrue(old_log.is_file())


if __name__ == "__main__":
    unittest.main()
