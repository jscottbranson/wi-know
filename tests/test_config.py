import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('generate', ROOT/'tools/generate_config.py')
generate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generate)

class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.settings = generate.load(ROOT/'config/settings.json')
    def test_following_alert(self):
        self.settings['following_alert']['enabled'] = True
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
        self.settings['gps'].update(enabled=True, rx_pin=5)
        self.settings['following_alert']['ignore_devices'] = [
            {'radio': 'ble', 'address_type': 1, 'address': 'AA:BB:CC:DD:EE:FF'}]
        settings, rules = generate.validate(self.settings)
        self.assertIn('DeviceIdentity{Radio::ble,1,{{0xAA', generate.emit(settings, rules))
        for field, value in [('distance_m', 0), ('delay_ms', -1), ('max_gap_ms', 0),
                             ('ignore_devices', [{'radio': 'wifi', 'address_type': 1, 'address': 'bad'}])]:
            invalid = copy.deepcopy(self.settings)
            invalid['following_alert'][field] = value
            with self.assertRaises(ValueError):
                generate.validate(invalid)

    def test_defaults(self):
        settings, rules = generate.validate(self.settings)
        self.assertEqual(settings['node']['role'], 'parent')
        self.assertEqual(rules, [])
    def test_role_requires_unique_nonzero_id(self):
        with self.assertRaises(ValueError):
            generate.validate(self.settings, 'child', 0)
        settings, _ = generate.validate(self.settings, 'child', 3)
        self.assertEqual(settings['node']['id'], 3)
    def test_worker_id_must_be_discoverable(self):
        self.settings['bus'].update(first_node_id=3, last_node_id=5)
        for role in ('child', 'follower'):
            for node_id in (2, 6):
                with self.assertRaisesRegex(ValueError, 'discovery range'):
                    generate.validate(copy.deepcopy(self.settings), role, node_id)
            for node_id in (3, 5):
                settings, _ = generate.validate(copy.deepcopy(self.settings), role, node_id)
                self.assertEqual(settings['node']['id'], node_id)
        generate.validate(copy.deepcopy(self.settings), 'parent', 0)
        self.settings['bus']['enabled'] = False
        generate.validate(self.settings, 'child', 2)
    def test_unwired_peripheral(self):
        self.settings['gps']['enabled'] = True
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
    def test_pin_conflict(self):
        self.settings['gps'].update(enabled=True, rx_pin=5)
        self.settings['buzzer'].update(enabled=True, pin=5)
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
    def test_bad_channels(self):
        for channels in ([1, 1], [52], [], [True], [[]], [{}]):
            settings = copy.deepcopy(self.settings)
            settings['wifi']['channels'] = channels
            with self.assertRaises(ValueError):
                generate.validate(settings)
    def test_malformed_rules(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = '.'
            doc = generate.load(ROOT/'rules/examples/lab.json')
            doc['rules'][0]['predicates'] = []
            (base/'bad.json').write_text(json.dumps(doc))
            with self.assertRaises(ValueError):
                generate.validate(self.settings, base=base)
    def test_lab_rule_emits_and_categories_disable(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = '.'
            doc = generate.load(ROOT/'rules/examples/lab.json')
            doc['rules'][0]['enabled'] = True
            (base/'lab.json').write_text(json.dumps(doc))
            settings, rules = generate.validate(self.settings, base=base)
            self.assertTrue(rules[0]['enabled'])
            self.assertIn('Field::ssid_prefix', generate.emit(settings, rules))
            self.settings['detection']['enabled_categories'] = []
            _, rules = generate.validate(self.settings, base=base)
            self.assertFalse(rules[0]['enabled'])
    def test_duplicate_keys(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder)/'duplicate.json'
            path.write_text('{"x":1,"x":2}')
            with self.assertRaises(ValueError):
                generate.load(path)
    def test_nonstandard_json_constants(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder)/'invalid.json'
            for value in ('NaN', 'Infinity', '-Infinity'):
                path.write_text('{"value":' + value + '}')
                with self.assertRaisesRegex(ValueError, 'nonstandard JSON'):
                    generate.load(path)
    def test_validation_does_not_mutate_settings(self):
        before = copy.deepcopy(self.settings)
        generate.validate(self.settings, 'child', 3)
        self.assertEqual(self.settings, before)
        with self.assertRaises(ValueError):
            generate.validate(self.settings, 'child', 0)
        self.assertEqual(self.settings, before)

    def test_malformed_rule_shapes_report_value_errors(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = '.'
            template = generate.load(ROOT/'rules/examples/lab.json')
            documents = [None, [], {'schema_version': True, 'rules': []},
                         {'schema_version': 1, 'rules': [None]}]
            for field, value in [('actions', ['buzzer', 'follow']),
                                 ('source', ['url', 'revision', 'license', 'reviewed']),
                                 ('predicates', [None]), ('predicates', [['field', 'value']])]:
                document = copy.deepcopy(template)
                document['rules'][0][field] = value
                documents.append(document)
            for document in documents:
                with self.subTest(document=document):
                    (base/'rule.json').write_text(json.dumps(document))
                    with self.assertRaises(ValueError):
                        generate.validate(self.settings, base=base)

    def test_text_prefix_limits_use_utf8_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = '.'
            document = generate.load(ROOT/'rules/examples/lab.json')
            for value, valid in [('a'*32, True), ('a'*33, False), ('é'*16, True),
                                 ('é'*17, False), ('a\x7f', False), ('a\x00', False),
                                 ('🔎', True)]:
                with self.subTest(value=value):
                    document['rules'][0]['predicates'][0]['value'] = value
                    (base/'rule.json').write_text(json.dumps(document))
                    if valid:
                        settings, rules = generate.validate(self.settings, base=base)
                        generate.emit(settings, rules).encode('ascii')
                    else:
                        with self.assertRaises(ValueError):
                            generate.validate(self.settings, base=base)

    def test_manifest_preserves_provenance_and_effective_enablement(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            rules_dir = base/'rules'
            rules_dir.mkdir()
            document = generate.load(ROOT/'rules/examples/lab.json')
            document['rules'][0]['enabled'] = True
            (rules_dir/'lab.json').write_text(json.dumps(document))
            self.settings['detection']['rules_directory'] = str(rules_dir)
            self.settings['detection']['enabled_categories'] = ['other']
            settings_file = base/'settings.json'
            settings_file.write_text(json.dumps(self.settings))
            output = base/'generated/settings.hpp'
            result = subprocess.run([sys.executable, str(ROOT/'tools/generate_config.py'),
                                     '--config', str(settings_file), '--output', str(output)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = generate.load(output.with_suffix('.rules.json'))
            self.assertEqual(manifest['rules'][0]['source'], document['rules'][0]['source'])
            self.assertEqual(manifest['rules'][0]['predicates'], document['rules'][0]['predicates'])
            self.assertFalse(manifest['rules'][0]['enabled'])
            self.assertIn('no enabled rules for categories: other', result.stdout)
    def test_unknown_setting_and_wrong_type(self):
        self.settings['gps']['enabled'] = 1
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
        self.settings['gps']['enabled'] = False
        self.settings['gps']['typo'] = 1
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
    def test_bus_timeout_and_uart_conflicts(self):
        self.settings['bus']['baud'] = 9600
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
        self.settings['bus']['baud'] = 115200
        self.settings['bus'].update(enabled=True, tx_pin=1, rx_pin=2, rts_pin=3)
        self.settings['gps'].update(enabled=True, uart=1, rx_pin=4)
        with self.assertRaises(ValueError):
            generate.validate(self.settings)

if __name__ == '__main__':
    unittest.main()
