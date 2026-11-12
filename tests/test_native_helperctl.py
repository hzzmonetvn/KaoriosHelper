"""Host integration tests execute the actual C++ CLI, never a model of it."""
import concurrent.futures
import json
import os
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
        subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", str(ROOT / "cli/helperctl/helperctl.cpp"), "-o", str(cls.binary)], check=True)

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

    def status(self):
        result = self.runctl('status', '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)['engines']['hma']

    def ack(self, event, generation=1, engine='hma', init=0):
        runtime = self.data / 'runtime'
        runtime.mkdir(exist_ok=True)
        (runtime / 'hma.ack.json').write_text(json.dumps(dict(schemaVersion=1, protocolVersion=1, engine=engine, processName='system_server', pid=os.getpid(), uid=1000, eventType=event, configGeneration=generation, initResult=init, errorCode=0, timestamp=123, detail='test')))

    def test_ack_truthfulness(self):
        self.assertEqual(self.setconfig('{}').returncode, 0)
        (self.data / 'classes.dex').write_text('local artifact')
        for event in ['APPLY_FAILED', 'PROCESS_LOADED', 'HOOK_REGISTERED']:
            self.ack(event)
            s = self.status()
            self.assertNotEqual(s['hookInitialized'], 'INITIALIZED')
            self.assertEqual(s['appliedGeneration'], 0)
        self.ack('HOOK_OBSERVED', generation=99)
        self.assertFalse(self.status()['runtimeObserved'])
        self.assertEqual(self.status()['details'], 'Runtime snapshot-generation acknowledgement unsupported by current engine')
        self.ack('HOOK_OBSERVED', engine='copg')
        self.assertFalse(self.status()['runtimeObserved'])
        self.ack('HOOK_OBSERVED', init=-1)
        self.assertFalse(self.status()['runtimeObserved'])
        self.ack('HOOK_OBSERVED')
        s = self.status()
        self.assertTrue(s['runtimeObserved'])
        self.assertEqual(s['status'], 'DEVICE_UNVERIFIED')
        self.assertEqual(s['appliedGeneration'], 1)
        self.assertFalse(s['deviceVerified'])
        self.assertNotEqual(self.runctl('verify', 'hma').returncode, 0)
        self.assertFalse((self.data / 'hma.verified').exists())
        (self.data / 'hma.verified').write_text('12345')
        self.assertFalse(self.status()['deviceVerified'])

    def test_validate_rejects_fifo_and_symlink(self):
        fifo = self.base / 'source.fifo'
        os.mkfifo(fifo)
        self.assertNotEqual(self.runctl('config', 'validate', 'hma', str(fifo)).returncode, 0)
        link = self.base / 'source.link'
        link.symlink_to(self.source('{}'))
        self.assertNotEqual(self.runctl('config', 'validate', 'hma', str(link)).returncode, 0)

    def test_kaorios_and_explicit_unsupported_sync(self):
        self.assertNotEqual(self.runctl('config', 'validate', 'unknown', self.source('{}')).returncode, 0)
        self.assertEqual(self.setconfig('{"user":true}', 'kaorios').returncode, 0)
        self.assertEqual(self.runctl('config', 'validate', 'kaorios').returncode, 0)
        self.assertEqual(self.runctl('config', 'status', 'kaorios').returncode, 0)
        result = self.runctl('config', 'sync', 'all')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no settings were changed', result.stderr)


if __name__ == '__main__':
    unittest.main()
