"""Exercise the pure adapter through its actual compiled C++ implementation."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SettingsAdaptersTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "adapter"
        source = Path(cls.build.name) / "adapter.cpp"
        source.write_text(r'''
#include "core/config/settings_adapter.hpp"
#include <iostream>
int main() {
    nlohmann::json request;
    std::cin >> request;
    try {
        std::string engine = request.at("engine");
        if (request.value("keys", false)) {
            std::cout << nlohmann::json(kaorios::settingsKeys(engine));
            return 0;
        }
        auto result = kaorios::adaptSettings(engine, request.value("existing", "{}"),
            request.at("values").get<std::map<std::string, std::string>>());
        std::cout << nlohmann::json({{"changed", result.has_value()},
            {"payload", result ? nlohmann::json(*result) : nlohmann::json(nullptr)}});
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what();
        return 1;
    }
}
''')
        subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-I", str(ROOT),
                        str(source), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def run_adapter(self, engine, values, existing=None):
        request = dict(engine=engine, values=values,
                       existing=existing if isinstance(existing, str) else json.dumps(existing or {}))
        return subprocess.run([str(self.binary)], input=json.dumps(request), text=True,
                              capture_output=True, timeout=5)

    def adapt(self, engine, values, existing=None):
        result = self.run_adapter(engine, values, existing)
        self.assertEqual(result.returncode, 0, result.stderr)
        response = json.loads(result.stdout)
        return json.loads(response['payload']) if response['changed'] else None

    def test_missing_empty_and_null_are_noop(self):
        for engine in ('pif', 'copg', 'hma', 'tee', 'kaorios'):
            self.assertIsNone(self.adapt(engine, {}))
        for empty in ('', '   \n', 'null', '\nnull\n'):
            self.assertIsNone(self.adapt('pif', {'kaorios_pif_json': empty}, {'manual': 1}))

    def test_semantically_unchanged_payload_is_noop(self):
        self.assertIsNone(self.adapt('pif', {'kaorios_pif_json': '{"MODEL":"fixture"}'}, {'MODEL': 'fixture'}))

    def test_pif_json_merges_present_fields_only(self):
        existing = {'MODEL': 'old', 'manual': {'kept': [1, 2]}, 'DEVICE': 'manual-device'}
        output = self.adapt('pif', {'kaorios_pif_json': '{"MODEL":"new","spoofBuild":"0"}'}, existing)
        self.assertEqual(output, dict(existing, MODEL='new', spoofBuild=0))

    def test_pif_property_format_and_validation(self):
        output = self.adapt('pif', {'kaorios_pif_json': '# comment\nMODEL=fixture\nspoofProps=true\nFIRST_API_LEVEL=31\n'})
        self.assertEqual(output, {'MODEL': 'fixture', 'spoofProps': 1, 'FIRST_API_LEVEL': '31'})
        for value in ('[]', '{broken', 'MODEL', '{"MODEL":4}', '{"spoofBuild":"maybe"}', '{"nested":{}}'):
            result = self.run_adapter('pif', {'kaorios_pif_json': value})
            self.assertNotEqual(result.returncode, 0, value)

    def test_copg_maps_game_fields_without_clobbering_manual_flags(self):
        existing = {'profiles': {'com.example.game': {'cpu_spoof': False, 'plt_hook': False, 'android_id_spoof': False},
                                 'com.example.manual': {'model': 'manual'}}, 'cpu_spoof': {'blacklist': ['com.example.excluded']}}
        raw = {'games': {'com.example.game': {'MODEL': 'fixture', 'BRAND': 'brand'}}, 'enabled': True}
        output = self.adapt('copg', {'kaorios_gameprops_json': json.dumps(raw)}, existing)
        self.assertEqual(output['profiles']['com.example.game'],
                         {'cpu_spoof': False, 'plt_hook': False, 'android_id_spoof': False, 'model': 'fixture', 'brand': 'brand'})
        self.assertEqual(output['profiles']['com.example.manual'], existing['profiles']['com.example.manual'])
        self.assertEqual(output['cpu_spoof'], existing['cpu_spoof'])
        self.assertTrue(output['enabled'])

    def test_copg_preserves_packages_object_schema(self):
        existing = {'packages': {'com.example.manual': {'enabled': False}}}
        output = self.adapt('copg', {'kaorios_gameprops_json': '{"games":{"com.example.new":{"MODEL":"fixture"}}}'}, existing)
        self.assertNotIn('profiles', output)
        self.assertEqual(output['packages']['com.example.manual'], {'enabled': False})
        self.assertEqual(output['packages']['com.example.new'], {'model': 'fixture'})

    def test_copg_disabled_toggle_and_unsupported_fields(self):
        self.assertFalse(self.adapt('copg', {'kaorios_spoof_gameprops': 'false'})['enabled'])
        for field in ('SDK_INT', 'SECURITY_PATCH', 'arbitrary'):
            value = json.dumps({'games': {'com.example.game': {field: 'fixture'}}})
            result = self.run_adapter('copg', {'kaorios_gameprops_json': value})
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unsupported', result.stderr)
        for value in ('{"games":[]}', '{"games":{"bad/package":{}}}', '{"games":{"com.example.game":{"MODEL":4}}}'):
            self.assertNotEqual(self.run_adapter('copg', {'kaorios_gameprops_json': value}).returncode, 0)

    def test_hma_simple_rules_templates_and_manual_scope(self):
        raw = {'configVersion': 1, 'webViewProtection': False,
               'scope': {'com.example.caller': {'extraAppList': ['com.example.hidden'],
                                              'applyTemplates': ['fixture'], 'excludeSystemApps': True}},
               'templates': {'fixture': {'appList': ['com.example.template']}}}
        existing = {'manual': 'kept', 'scope': {'com.example.manual': ['com.example.old']},
                    'global_hidden': ['com.example.global'], 'templates': {'fixture': {'manual': 'kept'}}}
        output = self.adapt('hma', {'kaorios_hma_config': json.dumps(raw)}, existing)
        self.assertEqual(output['scope']['com.example.caller'], ['com.example.hidden', 'com.example.template'])
        self.assertEqual(output['scope']['com.example.manual'], ['com.example.old'])
        self.assertEqual(output['global_hidden'], existing['global_hidden'])
        self.assertEqual(output['manual'], 'kept')
        self.assertEqual(output['templates'], existing['templates'])

    def test_normal_app_hma_default_is_explicitly_unsupported(self):
        raw = {'configVersion': 1, 'webViewProtection': True,
               'scope': {'com.example.caller': {'useWhitelist': False, 'excludeSystemApps': True,
                                              'extraAppList': ['com.example.hidden']}}}
        result = self.run_adapter('hma', {'kaorios_hma_config': json.dumps(raw)})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('HMA semantics unsupported', result.stderr)

    def test_hma_never_silently_drops_unrepresentable_rules(self):
        for rule in ({'useWhitelist': True}, {'extraOppositeAppList': ['com.example.visible']},
                     {'hideInstallationSource': True}, {'applySettingTemplates': ['x']},
                     {'applyPresets': ['x']}, {'applyTemplates': ['missing']}):
            raw = {'webViewProtection': False, 'scope': {'com.example.caller': rule}}
            self.assertNotEqual(self.run_adapter('hma', {'kaorios_hma_config': json.dumps(raw)}).returncode, 0, rule)
        for extra in ({'settingsTemplates': {'x': {'settingsList': []}}},
                      {'installSourceSpoof': ['com.example.app']}, {'configVersion': 2}):
            self.assertNotEqual(self.run_adapter('hma', {'kaorios_hma_config': json.dumps(extra)}).returncode, 0)

    def test_tee_active_xml_only_and_errors_are_redacted(self):
        xml = '<Keybox><Key algorithm="fixture"><PrivateKey>fixture-secret</PrivateKey><Certificate>fixture-cert</Certificate></Key></Keybox>'
        result = self.run_adapter('tee', {'kaorios_keybox_xml': xml}, '')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['payload'], xml)
        self.assertFalse(json.loads(self.run_adapter('tee', {'kaorios_keybox_xml': xml}, xml).stdout)['changed'])
        self.assertFalse(json.loads(self.run_adapter('tee', {'kaorios_keybox_hw_xml': xml}, '').stdout)['changed'])
        result = self.run_adapter('tee', {'kaorios_keybox_xml': '<Keybox>fixture-secret</Keybox>'}, '')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('fixture-secret', result.stderr + result.stdout)

    def test_kaorios_literal_scalar_storage_only(self):
        existing = {'schemaVersion': 1, 'manual': {'kept': True}, 'settingsGlobal': {'manual_extension': 'kept'}}
        output = self.adapt('kaorios', {'kaorios_spoof_photos': 'true', 'kaorios_advanced_features': '0',
                                      'kaorios_keybox_active_profile': 'rkp', 'kaorios_time': '999',
                                      'kaorios_pif_json': '{"MODEL":"fixture"}'}, existing)
        self.assertEqual(output['settingsGlobal'], {'manual_extension': 'kept', 'kaorios_spoof_photos': '1',
                                                    'kaorios_advanced_features': '0', 'kaorios_keybox_active_profile': 'rkp'})
        self.assertEqual(output['manual'], existing['manual'])
        for key, value in (('kaorios_spoof_photos', 'maybe'), ('kaorios_keybox_active_profile', 'other'),
                           ('kaorios_keybox_apply_all_mode', 'other'), ('kaorios_manager_package', 'bad/package')):
            self.assertNotEqual(self.run_adapter('kaorios', {key: value}).returncode, 0)

    def test_key_union_is_allowlisted_without_epoch_or_secure_namespace(self):
        result = subprocess.run([str(self.binary)], input=json.dumps(dict(engine='all', keys=True)),
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        keys = json.loads(result.stdout)
        for key in ('kaorios_pif_json', 'kaorios_gameprops_json', 'kaorios_hma_config', 'kaorios_keybox_xml'):
            self.assertIn(key, keys)
        for key in ('kaorios_time', 'per_apps_device_spoof', 'custom_spoof_profiles', 'spoof_trickystore_keybox'):
            self.assertNotIn(key, keys)
        self.assertEqual(len(keys), len(set(keys)))


if __name__ == '__main__':
    unittest.main()
