"""Host integration tests execute the actual C++ CLI, never a model of it."""
import concurrent.futures
import json
import os
import hashlib
import time
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeHelperCtlTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "helperctl"
        subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-DKAORIOS_TEST_FAULTS", str(ROOT / "cli/helperctl/helperctl.cpp"), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.data = self.base / "data"
        self.data.mkdir(mode=0o700)
        self.env = dict(os.environ, KAORIOS_DATA_DIR=str(self.data), KAORIOS_MOD_DIR=str(self.base))
        self.serial = 0
        self.process = subprocess.Popen(['kaorios-test-fixture', '-c', 'import sys; sys.stdin.buffer.read()'],
                                        executable=os.path.realpath('/proc/self/exe'), stdin=subprocess.PIPE)
        def close_process():
            self.process.stdin.close()
            self.process.wait(timeout=10)
        self.addCleanup(close_process)

    def runctl(self, *args):
        return subprocess.run([str(self.binary), *args], env=self.env, text=True, capture_output=True, timeout=10)

    def source(self, value):
        self.serial += 1
        p = self.base / f"source{self.serial}"
        p.write_text(value)
        return str(p)

    def setconfig(self, value, engine="hma"):
        return self.runctl("config", "set", engine, self.source(value))

    def test_malformed_and_type_validation(self):
        for bad in ['garbage', '{"MODEL":}', '[]', 'true', '{"a":1} trailing', '{"configVersion":"1"}', '{"scope":[]}']:
            self.assertNotEqual(self.setconfig(bad).returncode, 0, bad)
        self.assertNotEqual(self.setconfig('{"MODEL":42}', 'pif').returncode, 0)
        self.assertNotEqual(self.setconfig('{"cpu_spoof":[]}', 'copg').returncode, 0)
        for engine in ['pif', 'copg']:
            self.assertNotEqual(self.setconfig('{"enabled":1}', engine).returncode, 0)
        for version in ['2', '0', '-1', 'true', '"1"', '1.0']:
            self.assertNotEqual(self.setconfig('{"schemaVersion":' + version + '}', 'kaorios').returncode, 0)
        self.assertFalse((self.data / "hma.json").exists())

    def test_copg_consumed_profile_types(self):
        for profile in [
            {'cpu_spoof': 'yes'}, {'device_spoof': 1}, {'plt_hook': []},
            {'android_id_spoof': None}, {'enabled': 'true'}, {'model': 2},
            {'sdk_int': 2147483648}, {'sdk_int': -1}, {'props': {'ro.test': 2}},
        ]:
            payload = json.dumps({'profiles': {'com.example.game': profile}})
            self.assertNotEqual(self.setconfig(payload, 'copg').returncode, 0, profile)
        for payload in [{'profiles': []}, {'packages': [4]}, {'profiles': {'com.example.game': []}}]:
            self.assertNotEqual(self.setconfig(json.dumps(payload), 'copg').returncode, 0)
        payload = {'profiles': {'com.example.game': {'cpu_spoof': False, 'sdk_int': 35,
                    'props': {'ro.test': 'value'}, 'future_field': {'kept': True}}},
                   'user_extension': [1, 2]}
        self.assertEqual(self.setconfig(json.dumps(payload), 'copg').returncode, 0)
        self.assertEqual(json.loads((self.data / 'spoof.json').read_text()), payload)

    def test_preservation_rollback_and_generation(self):
        self.assertEqual(self.setconfig('{"user":{"a":1},"scope":{}}').returncode, 0)
        (self.data / "hma.verified").write_text("123")
        self.assertEqual(self.setconfig('{"configVersion":2}').returncode, 0)
        self.assertEqual(json.loads((self.data / "hma.json").read_text()), {"configVersion": 2})
        self.assertFalse((self.data / "hma.verified").exists())
        self.assertEqual(self.runctl('config', 'rollback', 'hma').returncode, 0)
        self.assertEqual(json.loads((self.data / 'hma.json').read_text()), {'user': {'a': 1}, 'scope': {}})
        result = self.runctl('config', 'get', 'hma')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout), {'user': {'a': 1}, 'scope': {}})
        self.assertEqual((self.data / 'hma.stored_gen').read_text(), '3\n')
        self.assertEqual((self.data / 'hma.requested_gen').read_text(), '3\n')
        self.assertEqual(self.runctl('reload', 'hma').returncode, 0)
        self.assertEqual((self.data / 'hma.requested_gen').read_text(), '3\n')

    def test_boot_preparation_renews_only_stale_requests(self):
        self.assertEqual(self.setconfig('{}', 'copg').returncode, 0)
        self.assertEqual(self.runctl('enable', 'copg').returncode, 0)
        self.ack('HOOK_OBSERVED')
        old = json.loads((self.data/'copg.request.json').read_text())
        self.assertEqual(self.runctl('prepare-boot').returncode, 0)
        self.assertEqual(json.loads((self.data/'copg.request.json').read_text()), old)
        self.assertTrue(self.status()['runtimeObserved'])
        (self.data/'copg.request.json').write_text(json.dumps({**old, 'bootId': 'previous-boot'}))
        self.assertEqual(self.runctl('prepare-boot').returncode, 0)
        request = json.loads((self.data/'copg.request.json').read_text())
        self.assertNotEqual(request['requestId'], old['requestId'])
        self.assertEqual(request['bootId'], Path('/proc/sys/kernel/random/boot_id').read_text().strip())
        self.assertEqual((self.data/'copg.stored_gen').read_text(), '1\n')
        self.assertFalse(self.status()['runtimeObserved'])
        self.assertTrue(self.status()['pendingReload'])
        self.ack('HOOK_OBSERVED')
        self.assertTrue(self.status()['runtimeObserved'])
        (self.data/'hma.json').write_text('{}')
        self.assertEqual(self.runctl('prepare-boot').returncode, 0)
        self.assertFalse((self.data/'hma.stored_gen').exists())
        self.assertFalse((self.data/'hma.request.json').exists())

    def test_boot_preparation_repairs_corrupt_requested_generation(self):
        for engine in ('hma', 'pif'):
            self.assertEqual(self.setconfig('{}', engine).returncode, 0)
        for corrupt in ('garbage', '-1', '+1', '1 extra', '18446744073709551616'):
            with self.subTest(generation=corrupt):
                old = {engine: json.loads((self.data/f'{engine}.request.json').read_text())
                       for engine in ('hma', 'pif')}
                (self.data/'hma.requested_gen').write_text(corrupt)
                (self.data/'pif.request.json').write_text(json.dumps({**old['pif'], 'bootId': 'previous-boot'}))
                result = self.runctl('prepare-boot')
                self.assertEqual(result.returncode, 0, result.stderr)
                for engine in ('hma', 'pif'):
                    request = json.loads((self.data/f'{engine}.request.json').read_text())
                    self.assertNotEqual(request['requestId'], old[engine]['requestId'])
                    self.assertEqual(request['bootId'], Path('/proc/sys/kernel/random/boot_id').read_text().strip())
                    self.assertEqual((self.data/f'{engine}.stored_gen').read_text(), '1\n')
                    self.assertEqual((self.data/f'{engine}.requested_gen').read_text(), '1\n')

    def test_boot_preparation_isolates_invalid_managed_engine(self):
        for engine in ('hma', 'pif'):
            self.assertEqual(self.setconfig('{}', engine).returncode, 0)
        for corrupt_leaf, corrupt in (('hma.stored_gen', 'broken'), ('hma.json', '{broken')):
            with self.subTest(leaf=corrupt_leaf):
                (self.data/'hma.stored_gen').write_text('1\n')
                (self.data/'hma.json').write_text('{}')
                old = {engine: json.loads((self.data/f'{engine}.request.json').read_text())
                       for engine in ('hma', 'pif')}
                for engine in ('hma', 'pif'):
                    (self.data/f'{engine}.request.json').write_text(json.dumps({**old[engine], 'bootId': 'previous-boot'}))
                (self.data/corrupt_leaf).write_text(corrupt)
                result = self.runctl('prepare-boot')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('hma', result.stderr)
                self.assertNotEqual(json.loads((self.data/'pif.request.json').read_text())['requestId'], old['pif']['requestId'])
                self.assertEqual(json.loads((self.data/'hma.request.json').read_text())['requestId'], old['hma']['requestId'])
                self.assertEqual((self.data/'pif.stored_gen').read_text(), '1\n')
                self.assertEqual((self.data/corrupt_leaf).read_text(), corrupt)

    def test_corrupt_generation_status_is_engine_local(self):
        self.assertEqual(self.setconfig('{}', 'copg').returncode, 0)
        self.assertEqual(self.runctl('enable', 'copg').returncode, 0)
        self.ack('HOOK_OBSERVED')
        self.assertEqual(self.setconfig('{}', 'pif').returncode, 0)
        for suffix in ('stored_gen', 'requested_gen'):
            for corrupt in ('broken', '-1', '+1', '1 extra', '18446744073709551616'):
                with self.subTest(leaf=suffix, generation=corrupt):
                    leaf = self.data/f'copg.{suffix}'
                    leaf.write_text(corrupt)
                    result = self.runctl('status', '--json')
                    self.assertEqual(result.returncode, 0, result.stderr)
                    states = json.loads(result.stdout)['engines']
                    state = states['copg']
                    self.assertEqual(state['status'], 'INVALID_CONFIG')
                    self.assertTrue(state['configValid'])
                    self.assertEqual(state['appliedGeneration'], 0)
                    for flag in ('processLoaded', 'hookRegistered', 'runtimeObserved', 'deviceVerified'):
                        self.assertFalse(state[flag])
                    self.assertTrue(state['pendingReload'])
                    self.assertIn('generation', state['lastError'])
                    self.assertNotIn(corrupt, state['lastError'] + state['details'])
                    self.assertEqual(states['pif']['storedGeneration'], 1)
                    self.assertNotEqual(states['pif']['status'], 'INVALID_CONFIG')
                    self.assertEqual(leaf.read_text(), corrupt)
                    leaf.write_text('1\n')

    def test_corrupt_stored_generation_requires_explicit_migration(self):
        payload = '{"profiles":{},"manual":{"keep":true}}\n'
        self.assertEqual(self.setconfig(payload, 'copg').returncode, 0)
        self.assertEqual(self.runctl('enable', 'copg').returncode, 0)
        self.settings_fixture({'kaorios_gameprops_json': '{"games":{"com.example.new":{"MODEL":"fixture"}}}'})
        lkg = '{"profiles":{"com.example.old":{}}}\n'
        (self.data/'spoof.json.lkg').write_text(lkg)
        for corrupt in ('broken', '18446744073709551616'):
            with self.subTest(generation=corrupt):
                (self.data/'copg.stored_gen').write_text('1\n')
                self.ack('HOOK_OBSERVED')
                self.assertTrue(self.status()['runtimeObserved'])
                old = (self.data/'copg.request.json').read_text()
                (self.data/'copg.stored_gen').write_text(corrupt)
                for args in (('config', 'set', 'copg', self.source('{}')),
                             ('config', 'rollback', 'copg'), ('config', 'sync', 'copg'),
                             ('reload', 'copg'), ('enable', 'copg'), ('disable', 'copg')):
                    self.assertNotEqual(self.runctl(*args).returncode, 0, args)
                    self.assertEqual((self.data/'copg.stored_gen').read_text(), corrupt)
                    self.assertEqual((self.data/'copg.request.json').read_text(), old)
                    self.assertEqual((self.data/'spoof.json').read_text(), payload)
                result = self.runctl('config', 'migrate', 'copg')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((self.data/'spoof.json').read_text(), payload)
                self.assertEqual((self.data/'copg.stored_gen').read_text(), '1\n')
                self.assertEqual((self.data/'copg.requested_gen').read_text(), '1\n')
                self.assertNotEqual((self.data/'copg.request.json').read_text(), old)
                self.assertFalse(self.status()['runtimeObserved'])
                self.assertEqual(self.status()['appliedGeneration'], 0)
                request = (self.data/'copg.request.json').read_text()
                self.assertEqual(self.runctl('config', 'migrate', 'copg').returncode, 0)
                self.assertEqual((self.data/'copg.request.json').read_text(), request)

    def test_migration_rejects_missing_payload_with_corrupt_metadata(self):
        for content in (None, ''):
            with self.subTest(payload=content):
                if content is not None:
                    (self.data/'pif.json').write_text(content)
                (self.data/'pif.stored_gen').write_text('broken')
                (self.data/'pif.requested_gen').write_text('1\n')
                state = self.status('pif')
                self.assertEqual(state['status'], 'INVALID_CONFIG')
                self.assertIn('Restore valid pif.json', state['details'])
                for engine in ('pif', 'all'):
                    result = self.runctl('config', 'migrate', engine)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn('pif', result.stderr)
                    self.assertEqual((self.data/'pif.stored_gen').read_text(), 'broken')
                    self.assertEqual((self.data/'pif.requested_gen').read_text(), '1\n')
                    self.assertFalse((self.data/'pif.request.json').exists())
                    self.assertFalse((self.data/'.native-transaction.json').exists())

    def test_storage_only_request_repair_guidance_uses_supported_command(self):
        payload = '{"schemaVersion":1,"manual":{"keep":true}}'
        self.assertEqual(self.setconfig(payload, 'kaorios').returncode, 0)
        (self.data/'kaorios.requested_gen').write_text('bad')
        state = self.status('kaorios')
        self.assertEqual(state['status'], 'INVALID_CONFIG')
        self.assertIn('config set kaorios FILE', state['details'])
        self.assertNotIn('prepare-boot', state['details'])
        self.assertNotIn('reload kaorios', state['details'])
        self.assertEqual(self.setconfig(payload, 'kaorios').returncode, 0)
        self.assertEqual(self.status('kaorios')['status'], 'INCOMPLETE_RUNTIME')
        self.assertEqual((self.data/'kaorios.requested_gen').read_text(), '2\n')

    def test_unmanaged_request_repair_guidance_requires_migration(self):
        (self.data/'pif.json').write_text('{}')
        (self.data/'pif.stored_gen').write_text('0\n')
        (self.data/'pif.requested_gen').write_text('junk')
        state = self.status('pif')
        self.assertEqual(state['status'], 'INVALID_CONFIG')
        self.assertIn('config migrate pif', state['details'])
        self.assertNotIn('prepare-boot', state['details'])
        self.assertEqual(self.runctl('config', 'migrate', 'pif').returncode, 0)
        self.assertEqual((self.data/'pif.stored_gen').read_text(), '1\n')
        self.assertEqual((self.data/'pif.requested_gen').read_text(), '1\n')
        self.assertEqual(self.status('pif')['status'], 'PENDING_RELOAD')

    def test_migration_rejects_invalid_payload_before_metadata_repair(self):
        (self.data/'hma.json').write_text('{broken')
        (self.data/'hma.stored_gen').write_text('broken')
        result = self.runctl('config', 'migrate', 'hma')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.data/'hma.stored_gen').read_text(), 'broken')
        self.assertFalse((self.data/'hma.request.json').exists())
        self.assertFalse((self.data/'.native-transaction.json').exists())

    def test_malformed_request_remains_pending(self):
        self.assertEqual(self.setconfig('{}', 'copg').returncode, 0)
        self.assertEqual(self.runctl('enable', 'copg').returncode, 0)
        value = self.ack('HOOK_OBSERVED')
        original = json.loads((self.data/'copg.request.json').read_text())
        for changes in ({'bootId': 1}, {'requestId': []}, {'notBeforeMillis': '1'},
                        {'notBeforeMillis': 0}, {'notBeforeMillis': 2**63}):
            (self.data/'copg.request.json').write_text(json.dumps({**original, **changes}))
            self.assertFalse(self.status()['runtimeObserved'], changes)
            self.assertEqual(self.runctl('prepare-boot').returncode, 0)
        self.assertFalse(self.status()['runtimeObserved'])
        self.assertEqual(value['configGeneration'], 1)

    def test_invalid_lkg_rejected(self):
        self.assertEqual(self.setconfig('{}').returncode, 0)
        (self.data / 'hma.json.lkg').write_text('invalid')
        self.assertNotEqual(self.runctl('config', 'rollback', 'hma').returncode, 0)
        self.assertEqual(json.loads((self.data / 'hma.json').read_text()), {})

    def test_internal_and_source_symlinks(self):
        outside = self.base / 'outside'
        outside.write_text('untouched')
        for name in ['hma.json', 'hma.json.lkg', '.config.lock', 'hma.stored_gen', 'hma.requested_gen', 'hma.verified', 'hma.reload_pending']:
            link = self.data / name
            if link.exists():
                link.unlink()
            link.symlink_to(outside)
            self.assertNotEqual(self.setconfig('{}').returncode, 0, name)
            if name in ['hma.json', '.config.lock']:
                self.assertNotEqual(self.runctl('config', 'get', 'hma').returncode, 0)
            self.assertEqual(outside.read_text(), 'untouched')
            link.unlink()
        source = self.base / 'link'
        source.symlink_to(self.source('{}'))
        self.assertNotEqual(self.runctl('config', 'set', 'hma', str(source)).returncode, 0)

    def test_directory_symlink_and_world_writable(self):
        alias = self.base / 'alias'
        alias.symlink_to(self.data, target_is_directory=True)
        self.env['KAORIOS_DATA_DIR'] = str(alias)
        self.assertNotEqual(self.setconfig('{}').returncode, 0)
        self.env['KAORIOS_DATA_DIR'] = str(self.data)
        self.data.chmod(0o777)
        self.assertNotEqual(self.setconfig('{}').returncode, 0)

    def test_concurrent_writers(self):
        sources = [self.source(json.dumps({f'user{i}': i})) for i in range(16)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda p: self.runctl('config', 'set', 'hma', p), sources))
        self.assertTrue(all(r.returncode == 0 for r in results), [r.stderr for r in results])
        self.assertEqual(len(json.loads((self.data / 'hma.json').read_text())), 1)
        self.assertEqual((self.data / 'hma.stored_gen').read_text(), '16\n')

    def status(self, engine='copg'):
        result = self.runctl('status', '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)['engines'][engine]

    def ack(self, event, generation=1, engine='copg', init=0, **overrides):
        runtime = self.data / 'runtime'
        runtime.mkdir(exist_ok=True)
        pid = self.process.pid
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        request = json.loads((self.data / 'copg.request.json').read_text())
        value = dict(schemaVersion=2, protocolVersion=2, engine=engine,
            processName=Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')[0].decode(),
            pid=pid, uid=os.geteuid(), eventType=event, configGeneration=generation,
            initResult=init, errorCode=0, timestamp=time.clock_gettime_ns(time.CLOCK_BOOTTIME)//1000000,
            bootId=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            processStartTicks=int(fields[19]), configHash=hashlib.sha256((self.data/'spoof.json').read_bytes()).hexdigest(),
            requestId=request['requestId'], detail='test')
        value.update(overrides)
        (runtime / 'copg.ack.json').write_text(json.dumps(value))
        return value

    def test_ack_truthfulness(self):
        self.assertEqual(self.setconfig('{}', 'copg').returncode, 0)
        self.assertEqual(self.runctl('enable', 'copg').returncode, 0)
        for event in ['APPLY_FAILED', 'PROCESS_LOADED', 'HOOK_REGISTERED']:
            self.ack(event)
            s = self.status()
            self.assertNotEqual(s['hookInitialized'], 'INITIALIZED')
            self.assertEqual(s['appliedGeneration'], 0)
        self.ack('HOOK_OBSERVED', generation=99)
        self.assertFalse(self.status()['runtimeObserved'])
        self.ack('HOOK_OBSERVED', engine='pif')
        self.assertFalse(self.status()['runtimeObserved'])
        self.ack('HOOK_OBSERVED', init=-1)
        self.assertFalse(self.status()['runtimeObserved'])
        self.ack('HOOK_OBSERVED', errorCode=3)
        self.assertFalse(self.status()['runtimeObserved'])
        self.assertEqual(self.status()['appliedGeneration'], 0)
        self.ack('HOOK_OBSERVED')
        s = self.status()
        self.assertTrue(s['runtimeObserved'])
        self.assertEqual(s['status'], 'DEVICE_UNVERIFIED')
        self.assertEqual(s['appliedGeneration'], 1)
        self.assertFalse(s['deviceVerified'])
        self.assertNotEqual(self.runctl('verify', 'copg').returncode, 0)
        self.assertFalse((self.data / 'copg.verified').exists())
        (self.data / 'copg.verified').write_text('12345')
        self.assertFalse(self.status()['deviceVerified'])

    def test_ack_session_and_snapshot_rejection(self):
        self.assertEqual(self.setconfig('{}', 'copg').returncode, 0)
        self.assertEqual(self.runctl('enable', 'copg').returncode, 0)
        for overrides in [
            {'pid': 2147483647}, {'bootId': '00000000-0000-0000-0000-000000000000'},
            {'processStartTicks': 1}, {'configHash': '0'*64}, {'timestamp': 1},
            {'timestamp': 2**63}, {'requestId': 'f'*32}, {'uid': os.geteuid()+1},
            {'schemaVersion': 1, 'protocolVersion': 1},
        ]:
            self.ack('HOOK_OBSERVED', **overrides)
            self.assertFalse(self.status()['runtimeObserved'], overrides)
            self.assertTrue(self.status()['pendingReload'])
        value = self.ack('HOOK_OBSERVED')
        self.assertTrue(self.status()['runtimeObserved'])
        for field in ['protocolVersion', 'uid', 'initResult', 'errorCode', 'timestamp', 'requestId', 'configHash']:
            invalid = dict(value)
            invalid.pop(field)
            (self.data/'runtime/copg.ack.json').write_text(json.dumps(invalid))
            self.assertFalse(self.status()['runtimeObserved'], field)
        self.ack('HOOK_OBSERVED')
        self.assertEqual(self.runctl('reload', 'copg').returncode, 0)
        self.assertFalse(self.status()['runtimeObserved'])
        self.assertTrue(self.status()['pendingReload'])
        self.ack('HOOK_OBSERVED')
        self.assertTrue(self.status()['runtimeObserved'])
        self.process.terminate()
        self.process.wait(timeout=10)
        self.assertFalse(self.status()['runtimeObserved'])

    def test_validate_rejects_fifo_and_symlink(self):
        fifo = self.base / 'source.fifo'
        os.mkfifo(fifo)
        self.assertNotEqual(self.runctl('config', 'validate', 'hma', str(fifo)).returncode, 0)
        link = self.base / 'source.link'
        link.symlink_to(self.source('{}'))
        self.assertNotEqual(self.runctl('config', 'validate', 'hma', str(link)).returncode, 0)

    def test_version_negotiates_actual_capabilities(self):
        result = self.runctl('version', '--json')
        self.assertEqual(result.returncode, 0)
        version = json.loads(result.stdout)
        self.assertEqual(version['moduleId'], 'kaorioshelper')
        self.assertEqual(version['protocolVersion'], 1)
        self.assertEqual(version['statusSchemaVersion'], 1)
        self.assertTrue(version['capabilities']['configSet'])
        for key in ('settingsGlobalSync', 'configMigration', 'recoverableConfigTransactions', 'runtimeAckV2'):
            self.assertTrue(version['capabilities'][key])
        for key in ('atomicMultiFileSnapshot', 'redactedConfigRead',
                    'structuredMutationErrors', 'deviceVerification', 'screenCapture'):
            self.assertFalse(version['capabilities'][key])

    def test_kaorios_and_settings_transport_missing(self):
        self.assertNotEqual(self.runctl('config', 'validate', 'unknown', self.source('{}')).returncode, 0)
        self.assertEqual(self.setconfig('{"user":true}', 'kaorios').returncode, 0)
        self.assertEqual(self.runctl('config', 'validate', 'kaorios').returncode, 0)
        self.assertEqual(self.runctl('config', 'status', 'kaorios').returncode, 0)
        self.env['KAORIOS_TEST_SETTINGS_BIN'] = str(self.base/'missing-settings')
        result = self.runctl('config', 'sync', 'all')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Settings command failed', result.stderr)
        self.assertFalse((self.data/'pif.json').exists())

    def settings_fixture(self, values=None, code=None):
        payload = self.base/'global-settings.json'
        payload.write_text(json.dumps(values or {}))
        transport = self.base/'settings'
        interpreter = os.path.realpath('/proc/self/exe')
        transport.write_text(f'#!{interpreter}\n' + (code or (
            'import json,sys\n'
            'from pathlib import Path\n'
            'assert sys.argv[1:3] == ["get", "global"]\n'
            'values=json.loads(Path(__file__).with_name("global-settings.json").read_text())\n'
            'print(values.get(sys.argv[3], "null"))\n')))
        transport.chmod(0o700)
        self.env['KAORIOS_TEST_SETTINGS_BIN'] = str(transport)

    def test_sync_actual_transport_merges_and_is_idempotent(self):
        self.assertEqual(self.setconfig('{"manual":{"keep":true},"MODEL":"old"}', 'pif').returncode, 0)
        self.settings_fixture({'kaorios_pif_json': '{"MODEL":"updated"}'})
        result = self.runctl('config', 'sync', 'pif')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.data/'pif.json').read_text()), {'manual': {'keep': True}, 'MODEL': 'updated'})
        self.assertEqual((self.data/'pif.stored_gen').read_text(), '2\n')
        self.assertEqual(self.runctl('config', 'sync', 'pif').returncode, 0)
        self.assertEqual((self.data/'pif.stored_gen').read_text(), '2\n')

    def test_copg_sync_requires_legacy_migration_before_publication(self):
        legacy = '{"enabled":false,"profiles":{"com.example.manual":{"model":"manual","cpu_spoof":false}},"extension":{"keep":true}}\n'
        (self.data/'COPG.json').write_text(legacy)
        self.settings_fixture()
        self.assertEqual(self.runctl('config', 'sync', 'copg').returncode, 0)
        self.assertFalse((self.data/'spoof.json').exists())
        self.settings_fixture({'kaorios_gameprops_json': '{"games":{"com.example.new":{"MODEL":"fixture"}}}'})
        result = self.runctl('config', 'sync', 'copg')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('migration', result.stderr)
        self.assertEqual((self.data/'COPG.json').read_text(), legacy)
        for leaf in ('spoof.json', 'copg.stored_gen', 'copg.request.json', '.native-transaction.json'):
            self.assertFalse((self.data/leaf).exists(), leaf)
        self.assertEqual(self.runctl('config', 'migrate', 'copg').returncode, 0)
        result = self.runctl('config', 'sync', 'copg')
        self.assertEqual(result.returncode, 0, result.stderr)
        config = json.loads((self.data/'spoof.json').read_text())
        self.assertEqual(config['profiles']['com.example.manual'], json.loads(legacy)['profiles']['com.example.manual'])
        self.assertEqual(config['profiles']['com.example.new'], {'model': 'fixture'})
        self.assertFalse(config['enabled'])
        self.assertEqual(config['extension'], {'keep': True})
        self.assertEqual((self.data/'COPG.json').read_text(), legacy)
        self.assertEqual((self.data/'copg.stored_gen').read_text(), '2\n')

    def test_sync_all_legacy_copg_rejection_preserves_other_engines(self):
        self.assertEqual(self.setconfig('{"MODEL":"old"}', 'pif').returncode, 0)
        (self.data/'COPG.json').write_text('{"profiles":{"com.example.manual":{"model":"manual"}}}')
        self.settings_fixture({'kaorios_pif_json': '{"MODEL":"updated"}',
                               'kaorios_gameprops_json': '{"games":{"com.example.new":{"MODEL":"fixture"}}}'})
        request = (self.data/'pif.request.json').read_text()
        result = self.runctl('config', 'sync', 'all')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads((self.data/'pif.json').read_text()), {'MODEL': 'old'})
        self.assertEqual((self.data/'pif.stored_gen').read_text(), '1\n')
        self.assertEqual((self.data/'pif.request.json').read_text(), request)
        self.assertFalse((self.data/'spoof.json').exists())
        self.assertFalse((self.data/'.native-transaction.json').exists())

    def test_sync_normalization_advances_generation_only_once(self):
        self.assertEqual(self.setconfig('{"spoofBuild":true}', 'pif').returncode, 0)
        self.settings_fixture({'kaorios_pif_json': '{"spoofBuild":"1"}'})
        result = self.runctl('config', 'sync', 'pif')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.data/'pif.stored_gen').read_text(), '2\n')
        self.assertIs(type(json.loads((self.data/'pif.json').read_text())['spoofBuild']), int)
        request = (self.data/'pif.request.json').read_text()
        self.assertEqual(self.runctl('config', 'sync', 'pif').returncode, 0)
        self.assertEqual((self.data/'pif.stored_gen').read_text(), '2\n')
        self.assertEqual((self.data/'pif.request.json').read_text(), request)

    def test_sync_all_prevalidates_and_preserves_on_error(self):
        self.assertEqual(self.setconfig('{"MODEL":"old"}', 'pif').returncode, 0)
        self.settings_fixture({'kaorios_pif_json': '{"MODEL":"updated"}', 'kaorios_hma_config': '{broken'})
        result = self.runctl('config', 'sync', 'all')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads((self.data/'pif.json').read_text()), {'MODEL': 'old'})
        self.assertEqual((self.data/'pif.stored_gen').read_text(), '1\n')
        self.assertNotIn('{broken', result.stderr)
        self.assertFalse((self.data/'.native-transaction.json').exists())
        self.settings_fixture({'kaorios_pif_json': '{"MODEL":"updated"}', 'kaorios_spoof_photos': '1'})
        result = self.runctl('config', 'sync', 'all')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.data/'kaorios.json').read_text())['settingsGlobal']['kaorios_spoof_photos'], '1')

    def test_settings_change_and_command_failure_never_publish(self):
        self.settings_fixture(code='import sys\nprint("payload-must-not-leak",file=sys.stderr)\nsys.exit(4)\n')
        result = self.runctl('config', 'sync', 'pif')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('payload-must-not-leak', result.stderr)
        self.assertFalse((self.data/'pif.json').exists())
        self.settings_fixture(code=(
            'from pathlib import Path\n'
            'counter=Path(__file__).with_name("counter")\n'
            'n=int(counter.read_text())+1 if counter.exists() else 1\n'
            'counter.write_text(str(n))\n'
            'print("{\\"MODEL\\":\\""+str(n)+"\\"}")\n'))
        result = self.runctl('config', 'sync', 'pif')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.data/'pif.json').exists())

    def test_native_transaction_recovers_each_publication_boundary(self):
        for boundary in range(6):
            with self.subTest(boundary=boundary):
                self.assertEqual(self.setconfig('{"configVersion":1}').returncode, 0)
                self.env['KAORIOS_TEST_TX_STOP_AFTER'] = str(boundary)
                result = self.setconfig('{"configVersion":2}')
                self.assertEqual(result.returncode, 86)
                self.assertTrue((self.data/'.native-transaction.json').exists())
                del self.env['KAORIOS_TEST_TX_STOP_AFTER']
                result = self.runctl('config', 'get', 'hma')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), {'configVersion': 2})
                self.assertEqual((self.data/'hma.stored_gen').read_text(), (self.data/'hma.requested_gen').read_text())
                self.assertFalse((self.data/'.native-transaction.json').exists())
                self.assertEqual(json.loads((self.data/'hma.json.lkg').read_text()), {'configVersion': 1})

    def test_migration_preserves_legacy_payload_and_does_not_repeat(self):
        legacy = '{"user":{"extension":true},"profiles":{}}\n'
        (self.data/'COPG.json').write_text(legacy)
        result = self.runctl('config', 'migrate', 'copg')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.data/'spoof.json').read_text(), legacy)
        self.assertEqual((self.data/'COPG.json').read_text(), legacy)
        self.assertEqual((self.data/'copg.stored_gen').read_text(), '1\n')
        self.assertEqual(self.runctl('config', 'migrate', 'all').returncode, 0)
        self.assertEqual((self.data/'copg.stored_gen').read_text(), '1\n')


if __name__ == '__main__':
    unittest.main()
