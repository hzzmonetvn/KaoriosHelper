import pathlib
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ScreenCaptureCapabilityTests(unittest.TestCase):
    def test_default_disabled_and_backend_unsupported(self):
        if shutil.which("cargo") is None:
            raise unittest.SkipTest("cargo not installed locally; runs in CI")
        subprocess.run(
            ["cargo", "test", "-p", "kaorios_common", "screen_capability"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )


if __name__ == "__main__":
    unittest.main()
