"""Exercise native snapshot readers and real engine metadata producers."""
import fcntl
import json
from pathlib import Path
import select
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <cassert>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include "core/config/runtime_snapshot.hpp"
#include "pif_engine.hpp"
#include "copg_engine.hpp"

using namespace kaorios;
namespace fs = std::filesystem;
const std::string firstToken(32, 'a');
const std::string nextToken(32, 'b');

struct CompanionCapture {
    int sockets[2];
    zygisk::internal::api_table table{};
    zygisk::Api api{&table};

    CompanionCapture() {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        table.impl = this;
        table.connectCompanion = [](void *self) {
            return dup(static_cast<CompanionCapture *>(self)->sockets[0]);
        };
        table.setOption = [](void *, zygisk::Option) {};
        const int response = 0;
        assert(write(sockets[1], &response, sizeof(response)) == sizeof(response));
    }
    ~CompanionCapture() { close(sockets[0]); close(sockets[1]); }
    bool hasAck(RuntimeAck &ack) {
        char buffer[8192];
        const ssize_t size = recv(sockets[1], buffer, sizeof(buffer), MSG_DONTWAIT);
        if (size < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); return false; }
        const std::string message(buffer, size);
        assert(message.starts_with("ack:"));
        assert(RuntimeAck::fromJson(nlohmann::json::parse(message.substr(4)), ack));
        return true;
    }
};

void emitResult(const nlohmann::json &value) {
    std::cout << "\nRESULT=" << value.dump() << '\n';
}

void publish(const std::string &dir, const std::string &engine, const std::string &bytes,
             uint64_t generation, const std::string &token) {
    NativeStore store(dir);
    const std::string filename = engine == "copg" ? "spoof.json" : engine + ".json";
    store.transaction({{filename, bytes},
        {engine + ".stored_gen", std::to_string(generation) + "\n"},
        {engine + ".requested_gen", std::to_string(generation) + "\n"},
        {engine + ".request.json", nlohmann::json{{"requestId", token},
            {"bootId", runtimeAckBootId()}, {"notBeforeMillis", runtimeAckNowMillis()}}.dump()}});
}

int main(int argc, char **argv) {
    assert(argc == 3);
    const std::string mode = argv[1], dir = argv[2];
    const std::string oldBytes = "{\"profiles\":{\"com.example.app\":{\"device_spoof\":false}}}";
    if (mode == "snapshot") {
        publish(dir, "copg", oldBytes, 7, firstToken);
        auto old = readRuntimeConfigSnapshot(dir, "copg", "spoof.json");
        assert(old.generation == 7 && old.bytes == oldBytes && old.requestId == firstToken);
        RuntimeAck ack;
        ack.engine = "copg";
        assert(bindRuntimeAckToSnapshot(ack, old));
        assert(runtimeAckMatchesCurrent(ack, "copg", 7, oldBytes, firstToken));
        publish(dir, "copg", oldBytes, 7, nextToken);
        auto reloaded = readRuntimeConfigSnapshot(dir, "copg", "spoof.json");
        assert(!reloaded.sameIdentity(old));
        assert(!runtimeAckMatchesCurrent(ack, "copg", reloaded.generation, reloaded.bytes, reloaded.requestId));
        publish(dir, "copg", "{}", 8, nextToken);
        auto current = readRuntimeConfigSnapshot(dir, "copg", "spoof.json");
        assert(old.generation == 7 && old.bytes == oldBytes && old.requestId == firstToken);
        assert(!runtimeAckMatchesCurrent(ack, "copg", current.generation, current.bytes, current.requestId));
        emitResult(ack.toJson());
    } else if (mode == "legacy") {
        {
            NativeStore store(dir);
            store.write("spoof.json", oldBytes);
            store.write("copg.enabled", "");
        }
        auto legacy = readRuntimeConfigSnapshot(dir, "copg", "spoof.json");
        assert(legacy.bytes == oldBytes && legacy.generation == 0 && legacy.requestId.empty());
        RuntimeAck ack;
        ack.engine = "copg";
        assert(!bindRuntimeAckToSnapshot(ack, legacy));
        CompanionCapture companion;
        CopgEngine engine(dir);
        engine.onLoad(&companion.api, nullptr);
        engine.preAppSpecialize(nullptr, "com.example.app");
        engine.postAppSpecialize(nullptr, "com.example.app");
        assert(!companion.hasAck(ack));
        assert(!fs::exists(dir + "/runtime/copg.ack.json"));
        publish(dir, "copg", oldBytes, 7, firstToken);
        { NativeStore store(dir); store.remove("spoof.json"); store.write("COPG.json", oldBytes); }
        auto fallback = readRuntimeConfigSnapshot(dir, "copg", "spoof.json");
        assert(fallback.bytes == oldBytes && fallback.generation == 0);
        assert(!bindRuntimeAckToSnapshot(ack, fallback));
        emitResult(nlohmann::json::object());
    } else if (mode == "copg") {
        publish(dir, "copg", oldBytes, 7, firstToken);
        { NativeStore store(dir); store.write("copg.enabled", ""); }
        CompanionCapture companion;
        CopgEngine engine(dir);
        engine.onLoad(&companion.api, nullptr);
        engine.preAppSpecialize(nullptr, "com.example.app");
        publish(dir, "copg", "{}", 8, nextToken);
        engine.postAppSpecialize(nullptr, "com.example.app");
        RuntimeAck ack;
        assert(companion.hasAck(ack));
        assert(ack.configGeneration == 7 && ack.configHash == configSha256(oldBytes) && ack.requestId == firstToken);
        assert(ack.eventType == AckEventType::PROCESS_LOADED);
        emitResult(ack.toJson());
    } else if (mode == "pif") {
        const std::string pifBytes = "{\"MODEL\":\"fixture\",\"spoofProps\":1,\"spoofBuild\":0}";
        publish(dir, "pif", pifBytes, 7, firstToken);
        CompanionCapture companion;
        PifEngine engine(dir);
        engine.onLoad(&companion.api, nullptr);
        engine.preAppSpecialize(nullptr, "com.google.android.gms.unstable");
        publish(dir, "pif", "{}", 8, nextToken);
        engine.postAppSpecialize(nullptr, "com.google.android.gms.unstable");
        RuntimeAck ack;
        assert(companion.hasAck(ack));
        assert(ack.configGeneration == 7 && ack.configHash == configSha256(pifBytes) && ack.requestId == firstToken);
        assert(ack.eventType == AckEventType::HOOK_INITIALIZED);
        emitResult(ack.toJson());
    } else if (mode == "copg-reload") {
        publish(dir, "copg", oldBytes, 7, firstToken);
        CompanionCapture companion;
        CopgEngine engine(dir);
        engine.onLoad(&companion.api, nullptr);
        engine.preAppSpecialize(nullptr, "com.example.app");
        struct stat before{};
        assert(stat((dir + "/spoof.json").c_str(), &before) == 0);
        publish(dir, "copg", "{}", 8, nextToken);
        const timespec sameTime[2] = {before.st_atim, before.st_mtim};
        assert(utimensat(AT_FDCWD, (dir + "/spoof.json").c_str(), sameTime, 0) == 0);
        assert(!engine.shouldHandleApp("com.example.app", 10000));
        // A newer loader snapshot must not relabel an already-selected payload.
        engine.postAppSpecialize(nullptr, "com.example.app");
        RuntimeAck ack;
        assert(companion.hasAck(ack));
        assert(ack.configGeneration == 7 && ack.configHash == configSha256(oldBytes));
        assert(ack.requestId == firstToken);
        emitResult(ack.toJson());
    } else if (mode == "copg-invalid") {
        publish(dir, "copg", oldBytes, 7, firstToken);
        CompanionCapture companion;
        CopgEngine engine(dir);
        engine.onLoad(&companion.api, nullptr);
        engine.preAppSpecialize(nullptr, "com.example.app");
        publish(dir, "copg", "{bad", 8, nextToken);
        engine.preAppSpecialize(nullptr, "com.example.app");
        engine.postAppSpecialize(nullptr, "com.example.app");
        RuntimeAck ack;
        assert(companion.hasAck(ack));
        assert(ack.configGeneration == 7 && ack.configHash == configSha256(oldBytes));
        assert(ack.requestId == firstToken);
        emitResult(ack.toJson());
    } else if (mode == "read" || mode == "read-lock") {
        if (mode == "read-lock") std::cout << "READY\n" << std::flush;
        auto snapshot = readRuntimeConfigSnapshot(dir, "copg", "spoof.json");
        RuntimeAck ack;
        ack.engine = "copg";
        emitResult(nlohmann::json{{"generation", snapshot.generation},
            {"bound", bindRuntimeAckToSnapshot(ack, snapshot)}, {"bytes", snapshot.bytes}});
    } else {
        return 1;
    }
}
'''


class RuntimeSnapshotTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        source = Path(cls.build.name) / 'snapshot.cpp'
        source.write_text(HARNESS)
        cls.binary = Path(cls.build.name) / 'snapshot'
        subprocess.run([
            'g++', '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter',
            '-I', str(ROOT), '-I', str(ROOT / 'include'),
            '-I', str(ROOT / 'third_party/zygisk'), '-I', str(ROOT / 'third_party/jni'),
            '-I', str(ROOT / 'zygisk/include'), '-I', str(ROOT / 'engines/pif'),
            '-I', str(ROOT / 'engines/copg'), str(source),
            str(ROOT / 'engines/pif/pif_engine.cpp'), str(ROOT / 'engines/copg/copg_engine.cpp'),
            '-o', str(cls.binary),
        ], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.data = Path(self.tmp.name)
        self.data.chmod(0o700)

    def run_harness(self, mode):
        name = 'com.google.android.gms.unstable' if mode == 'pif' else 'com.example.app'
        result = subprocess.run([name, mode, str(self.data)], executable=str(self.binary),
                                text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return json.loads(result.stdout.rsplit('RESULT=', 1)[1])

    def test_snapshot_preserves_loaded_generation_and_reload_token(self):
        self.assertEqual(self.run_harness('snapshot')['configGeneration'], 7)

    def test_legacy_configuration_never_emits_current_ack(self):
        self.run_harness('legacy')

    def test_copg_ack_uses_pre_specialization_snapshot(self):
        ack = self.run_harness('copg')
        self.assertEqual(ack['configGeneration'], 7)
        self.assertEqual(ack['eventType'], 'PROCESS_LOADED')

    def test_pif_ack_uses_pre_specialization_snapshot(self):
        ack = self.run_harness('pif')
        self.assertEqual(ack['configGeneration'], 7)
        self.assertEqual(ack['eventType'], 'HOOK_INITIALIZED')

    def test_copg_same_mtime_reload_does_not_relabel_selected_payload(self):
        self.assertEqual(self.run_harness('copg-reload')['configGeneration'], 7)

    def test_copg_rejected_payload_retains_previous_metadata(self):
        self.assertEqual(self.run_harness('copg-invalid')['configGeneration'], 7)

    def test_snapshot_recovers_durable_transaction_before_read(self):
        (self.data / 'spoof.json').write_text('{"old":true}')
        (self.data / 'copg.stored_gen').write_text('6')
        request = {'requestId': 'a' * 32,
                   'bootId': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                   'notBeforeMillis': 1}
        journal = {'schemaVersion': 1, 'files': {
            'spoof.json': '{}', 'copg.stored_gen': '7', 'copg.requested_gen': '7',
            'copg.request.json': json.dumps(request)}}
        (self.data / '.native-transaction.json').write_text(json.dumps(journal))
        result = self.run_harness('read')
        self.assertEqual(result, {'generation': 7, 'bound': True, 'bytes': '{}'})
        self.assertFalse((self.data / '.native-transaction.json').exists())

    def test_snapshot_waits_for_management_transaction_lock(self):
        (self.data / 'spoof.json').write_text('{}')
        with (self.data / '.config.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            process = subprocess.Popen(['com.example.app', 'read-lock', str(self.data)],
                                       executable=str(self.binary), text=True,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                self.assertEqual(process.stdout.readline(), 'READY\n')
                self.assertEqual(select.select([process.stdout], [], [], 0.1)[0], [])
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
                try:
                    stdout, stderr = process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    raise
        self.assertEqual(process.returncode, 0, stderr)
        self.assertEqual(json.loads(stdout.rsplit('RESULT=', 1)[1])['generation'], 0)

    def test_snapshot_rejects_symlink_metadata(self):
        (self.data / 'spoof.json').write_text('{}')
        (self.data / 'other').write_text('7')
        (self.data / 'copg.stored_gen').symlink_to(self.data / 'other')
        result = subprocess.run(['com.example.app', 'read', str(self.data)],
                                executable=str(self.binary), text=True,
                                capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('RESULT=', result.stdout)

    def test_invalid_or_stale_request_metadata_is_unbound(self):
        (self.data / 'spoof.json').write_text('{}')
        (self.data / 'copg.stored_gen').write_text('7\n')
        (self.data / 'copg.requested_gen').write_text('7\n')
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        cases = [
            {}, {'requestId': 'a' * 32, 'bootId': boot, 'notBeforeMillis': 0},
            {'requestId': 'a' * 32, 'bootId': 'old-boot', 'notBeforeMillis': 1},
            {'requestId': 'bad', 'bootId': boot, 'notBeforeMillis': 1},
            {'requestId': 'a' * 32, 'bootId': boot, 'notBeforeMillis': 2**64 - 1},
            {'requestId': 'a' * 32, 'bootId': boot, 'notBeforeMillis': -1},
        ]
        for request in cases:
            with self.subTest(request=request):
                (self.data / 'copg.request.json').write_text(json.dumps(request))
                result = self.run_harness('read')
                self.assertEqual(result['generation'], 0)
                self.assertFalse(result['bound'])
                self.assertEqual(result['bytes'], '{}')
        (self.data / 'copg.request.json').unlink()
        self.assertEqual(self.run_harness('read')['generation'], 0)

    def test_malformed_generation_is_not_inferred_from_payload(self):
        (self.data / 'spoof.json').write_text('{}')
        for generation in ('0', '-1', '+7', '7 extra', '18446744073709551616', ''):
            with self.subTest(generation=generation):
                (self.data / 'copg.stored_gen').write_text(generation)
                (self.data / 'copg.requested_gen').write_text('7')
                result = self.run_harness('read')
                self.assertEqual(result['generation'], 0)
                self.assertFalse(result['bound'])


if __name__ == '__main__':
    unittest.main()
