"""Integration fixtures use the actual native CLI and a live native ACK producer."""

import atexit
import hashlib
import json
import os
import select
import subprocess
import tempfile
import unittest
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@lru_cache(maxsize=1)
def native_binaries():
    build = tempfile.TemporaryDirectory(prefix="native control ; ")
    atexit.register(build.cleanup)
    root = Path(build.name)
    control = root / "helperctl"
    producer = root / "ack-producer"
    source = root / "producer.cpp"
    source.write_text('''#include "core/config/runtime_snapshot.hpp"
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 3) return 1;
    auto snapshot = kaorios::readRuntimeConfigSnapshot(argv[1], "copg", "spoof.json");
    kaorios::RuntimeAck ack;
    ack.engine = "copg";
    ack.eventType = kaorios::stringToAckEventType(argv[2]);
    if (!kaorios::bindRuntimeAckToSnapshot(ack, snapshot) ||
        !kaorios::writeRuntimeAckDirect(std::string(argv[1]) + "/runtime", ack)) return 2;
    std::cout << "ready\\n" << std::flush;
    std::cin.get();
}
''')
    compiler = ["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror"]
    subprocess.run([*compiler, str(ROOT / "cli/helperctl/helperctl.cpp"), "-o", str(control)], check=True)
    subprocess.run([*compiler, "-I", str(ROOT), str(source), "-o", str(producer)], check=True)
    return control, producer


class NativeControlCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.control, cls.ack_producer = native_binaries()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.data_dir = self.base / "data"
        self.data_dir.mkdir(mode=0o700)
        (self.base / "module.prop").write_text("id=kaorioshelper\nversion=v2.0.0-fixture\n")
        self.env = dict(os.environ, KAORIOS_DATA_DIR=str(self.data_dir), KAORIOS_MOD_DIR=str(self.base))

    def native(self, *arguments):
        result = subprocess.run([str(self.control), *arguments], env=self.env, text=True,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def configure(self, engine="copg", content="{}"):
        source = self.base / "source.json"
        source.write_text(content)
        self.native("config", "set", engine, str(source))
        if engine in ("copg", "tee"):
            self.native("enable", engine)

    def produce_ack(self, event="HOOK_OBSERVED"):
        process = subprocess.Popen([str(self.ack_producer), str(self.data_dir), event],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)

        def close():
            process.stdin.close()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
            process.stdout.close()
            process.stderr.close()

        self.addCleanup(close)
        self.assertTrue(select.select([process.stdout], [], [], 10)[0], "ACK producer did not respond")
        self.assertEqual(process.stdout.readline(), "ready\n")
        ack = json.loads((self.data_dir / "runtime/copg.ack.json").read_text())
        self.assertEqual(ack["pid"], process.pid)
        self.assertEqual(ack["configHash"], hashlib.sha256((self.data_dir / "spoof.json").read_bytes()).hexdigest())
        return process, ack
