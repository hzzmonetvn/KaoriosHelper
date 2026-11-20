"""Exercise Rust settings_adapter via cargo test."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SettingsAdaptersTest(unittest.TestCase):
    def test_rust_settings_adapters(self):
        if shutil.which("cargo") is None:
            raise unittest.SkipTest("cargo not installed locally; runs in CI")
        subprocess.run(
            ["cargo", "test", "-p", "kaorios_common", "settings_adapter"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )


if __name__ == "__main__":
    unittest.main()
