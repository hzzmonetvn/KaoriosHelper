"""Exercise Rust runtime_snapshot & engines via cargo test."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RuntimeSnapshotTest(unittest.TestCase):
    def test_rust_snapshot_and_engines(self):
        if shutil.which("cargo") is None:
            raise unittest.SkipTest("cargo not installed locally; runs in CI")
        subprocess.run(
            ["cargo", "test", "--workspace"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )


if __name__ == "__main__":
    unittest.main()
