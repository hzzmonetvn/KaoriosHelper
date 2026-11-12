import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ScreenCaptureCapabilityTests(unittest.TestCase):
    def test_default_disabled_and_backend_unsupported(self):
        source = '''
#include "engines/screen/screen_capability.hpp"
#include <string_view>
using kaorios::ScreenCaptureCapability;
static_assert(!ScreenCaptureCapability::default_enabled);
static_assert(!ScreenCaptureCapability::supported);
static_assert(std::string_view(ScreenCaptureCapability::backend) == "unsupported");
static_assert(std::string_view(ScreenCaptureCapability::status) == "BLOCKED");
int main() {}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / "screen-capability"
            subprocess.run(
                ["c++", "-std=c++17", "-x", "c++", "-I", str(ROOT),
                 "-o", str(executable), "-"],
                input=source, text=True, check=True, capture_output=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
