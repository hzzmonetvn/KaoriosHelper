"""Exercise the real settings subprocess transport with bounded local fixtures."""
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SettingsCommandTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        root = Path(cls.build.name)
        source = root / "transport.cpp"
        source.write_text('''#include "core/config/settings_command.hpp"
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        if (std::string(argv[1]) == "snapshot") {
            kaorios::captureGlobalSettings({"first", "second", "third"});
        } else {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::stoi(argv[1]));
            std::cout << kaorios::readGlobalSetting(argv[2], deadline);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what();
        return 1;
    }
}
''')
        cls.binary = root / "transport"
        subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        "-DKAORIOS_TEST_FAULTS", "-I", str(ROOT), str(source),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixture = self.root / "settings command"
        self.env = dict(os.environ, KAORIOS_TEST_SETTINGS_BIN=str(self.fixture))

    def run_transport(self, script, key="test_key", timeout="5000"):
        self.fixture.write_text("#!/usr/bin/python3 -I\n" + script)
        self.fixture.chmod(0o700)
        return subprocess.run([str(self.binary), timeout, key], env=self.env,
                              capture_output=True, text=True, timeout=10)

    def test_argument_boundaries_are_preserved(self):
        key = 'key with spaces; $(touch injected) "quoted"'
        result = self.run_transport("import json, sys\nprint(json.dumps(sys.argv[1:]))\n", key)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), ["get", "global", key])
        self.assertFalse((self.root / "injected").exists())

    def test_only_one_record_terminator_is_removed(self):
        result = self.run_transport('import os\nos.write(1, b"fixture\\n\\n")\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "fixture\n")

    def test_nonzero_exit_redacts_both_output_streams(self):
        result = self.run_transport('import os\nos.write(1, b"payload-canary")\nos.write(2, b"payload-canary")\nraise SystemExit(7)\n')
        self.assertEqual(result.returncode, 1)
        self.assertIn("Settings command failed", result.stderr)
        self.assertNotIn("payload-canary", result.stdout + result.stderr)

    def test_oversized_output_is_rejected_without_payload(self):
        result = self.run_transport('import sys\nsys.stdout.buffer.write(b"payload-canary" * (2 * 1024 * 1024))\nsys.stdout.buffer.flush()\n')
        self.assertEqual(result.returncode, 1)
        self.assertIn("exceeded bounds", result.stderr)
        self.assertNotIn("payload-canary", result.stdout + result.stderr)

    def test_aggregate_snapshot_limit_is_enforced(self):
        result = self.run_transport('import sys\nsys.stdout.buffer.write(b"x" * (11 * 1024 * 1024) + b"\\n")\nsys.stdout.buffer.flush()\n', timeout="snapshot")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "Settings snapshot too large")

    def test_deadline_still_applies_after_stdout_closes(self):
        started = time.monotonic()
        result = self.run_transport("import os, signal\nos.close(1)\nsignal.pause()\n", timeout="300")
        self.assertEqual(result.returncode, 1)
        self.assertIn("exceeded bounds", result.stderr)
        self.assertLess(time.monotonic() - started, 4)

    def test_deadline_kills_descendants_holding_stdout(self):
        child_file = self.root / "child.pid"
        self.env["SETTINGS_TEST_CHILD"] = str(child_file)
        started = time.monotonic()
        result = self.run_transport('''import os, signal
pid = os.fork()
if pid == 0:
    signal.pause()
else:
    with open(os.environ["SETTINGS_TEST_CHILD"], "w") as stream:
        stream.write(str(pid))
    os.waitpid(pid, 0)
''', timeout="1000")
        self.assertEqual(result.returncode, 1)
        self.assertIn("exceeded bounds", result.stderr)
        self.assertLess(time.monotonic() - started, 4)
        self.assertTrue(child_file.is_file(), "fixture did not create its descendant")
        child = int(child_file.read_text())

        def live():
            try:
                state = Path(f"/proc/{child}/stat").read_text().rsplit(")", 1)[1].split()[0]
                return state not in ("Z", "X", "x")
            except FileNotFoundError:
                return False

        try:
            deadline = time.monotonic() + 2
            while live() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertFalse(live(), "settings descendant survived the deadline")
        finally:
            if live():
                os.kill(child, signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()
