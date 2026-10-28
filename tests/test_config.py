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
        self.settings['node']['role'] = 'parent'
    def test_universal_profiles_and_strap_conflicts(self):
        for name in ('settings.json', 'settings.universal.example.json'):
            settings, rules = generate.validate(generate.load(ROOT/'config'/name))
            self.assertEqual(settings['node']['role'], 'universal')
            header = generate.emit(settings, rules)
            self.assertIn('universal_enabled = true', header)
            self.assertIn('universal_parent_rx_pin = 3', header)
            self.assertIn('universal_worker_rx_pin = 12', header)
            self.assertIn('universal_strap_pin = 10', header)
        settings = generate.load(ROOT/'config/settings.universal.example.json')
        settings['gps']['rx_pin'] = 10
        with self.assertRaisesRegex(ValueError, 'GPIO 10 used'):
            generate.validate(settings)
        settings['gps']['rx_pin'] = 3
        with self.assertRaisesRegex(ValueError, 'GPIO 3 used'):
            generate.validate(settings)

    def test_universal_rejects_invalid_identity_and_profile(self):
        settings = generate.load(ROOT/'config/settings.json')
        for change in ({'node': {'role': 'universal', 'id': 1}},
                       {'bus': {**settings['bus'], 'enabled': False}}):
            with self.assertRaises(ValueError):
                generate.validate({**settings, **change})
        settings, _ = generate.validate(settings)
        settings['universal']['strap_pin'] = 6
        with self.assertRaisesRegex(ValueError, 'verified GPIO10'):
            generate.validate(settings)

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

    def test_standalone_wiring_uses_all_header_pins(self):
        settings, rules = generate.validate(generate.load(ROOT/'config/settings.standalone-wiring.example.json'))
        self.assertEqual(settings['node'], {'role': 'parent', 'id': 0})
        self.assertTrue(settings['standalone']['enabled'])
        self.assertFalse(settings['bus']['enabled'])
        header = generate.emit(settings, rules)
        assignments = {
            'display': {'sda_pin': 23, 'scl_pin': 24},
            'gps': {'rx_pin': 12},
            'sd': {'sck_pin': 8, 'mosi_pin': 10, 'miso_pin': 9, 'cs_pin': 11},
            'buzzer': {'pin': 7},
            'signal_trend': {'increasing_pin': 1, 'decreasing_pin': 0, 'steady_pin': 25},
        }
        pins = []
        for section, expected in assignments.items():
            self.assertTrue(settings[section]['enabled'])
            self.assertIn(f'{section}_enabled = true;', header)
            for key, pin in expected.items():
                self.assertEqual(settings[section][key], pin)
                self.assertIn(f'{section}_{key} = {pin};', header)
                pins.append(settings[section][key])
        # D0..D10 are the exposed header GPIOs; USB, battery and BOOT stay free.
        self.assertEqual(set(pins), {1, 0, 25, 7, 23, 24, 11, 12, 8, 9, 10})
        self.assertEqual(len(pins), len(set(pins)))
        self.assertEqual(settings['gps']['uart'], 0)
        self.assertEqual(settings['gps']['tx_pin'], -1)
        self.assertEqual(settings['gps']['baud'], 9600)
        self.assertEqual(settings['sd']['clock_khz'], 4000)

    def test_standalone_priority_peripheral_combinations(self):
        example = generate.load(ROOT/'config/settings.standalone-wiring.example.json')
        priority = ('display', 'gps', 'sd', 'buzzer', 'signal_trend')
        for count in range(len(priority) + 1):
            with self.subTest(peripherals=priority[:count]):
                settings = copy.deepcopy(example)
                for index, section in enumerate(priority):
                    settings[section]['enabled'] = index < count
                validated, rules = generate.validate(settings)
                header = generate.emit(validated, rules)
                for index, section in enumerate(priority):
                    enabled = 'true' if index < count else 'false'
                    self.assertIn(f'{section}_enabled = {enabled};', header)

    def test_standalone_gps_sd_require_assigned_unique_pins(self):
        example = generate.load(ROOT/'config/settings.standalone-wiring.example.json')
        for section, keys in [('gps', ('rx_pin',)),
                              ('sd', ('sck_pin', 'mosi_pin', 'miso_pin', 'cs_pin'))]:
            for key in keys:
                with self.subTest(section=section, key=key):
                    settings = copy.deepcopy(example)
                    settings[section][key] = -1
                    with self.assertRaisesRegex(ValueError, 'assign GPIO'):
                        generate.validate(settings)
        for section, key, pin in [('gps', 'rx_pin', 9), ('sd', 'cs_pin', 7),
                                  ('sd', 'mosi_pin', 23), ('gps', 'tx_pin', 11)]:
            with self.subTest(section=section, key=key, pin=pin):
                settings = copy.deepcopy(example)
                settings[section][key] = pin
                with self.assertRaisesRegex(ValueError, f'GPIO {pin} used'):
                    generate.validate(settings)

    def test_standalone_optional_indicators(self):
        example = generate.load(ROOT/'config/settings.standalone-wiring.example.json')
        keys = ('increasing_pin', 'decreasing_pin', 'steady_pin')
        for mask in range(8):
            with self.subTest(mask=mask):
                settings = copy.deepcopy(example)
                for index, key in enumerate(keys):
                    if not mask & (1 << index):
                        settings['signal_trend'][key] = -1
                validated, rules = generate.validate(settings)
                header = generate.emit(validated, rules)
                for key in keys:
                    self.assertIn(f'signal_trend_{key} = {settings["signal_trend"][key]}', header)
        # An omitted LED releases its GPIO for another peripheral.
        settings = copy.deepcopy(example)
        settings['signal_trend']['steady_pin'] = -1
        settings['gps'].update(enabled=True, rx_pin=25)
        generate.validate(settings)
        settings['signal_trend']['steady_pin'] = 25
        with self.assertRaisesRegex(ValueError, 'GPIO 25 used'):
            generate.validate(settings)
        for section, key in [('display', 'sda_pin'), ('buzzer', 'pin')]:
            settings = copy.deepcopy(example)
            settings[section][key] = -1
            with self.assertRaisesRegex(ValueError, 'assign GPIO'):
                generate.validate(settings)

    def test_standalone(self):
        self.settings['standalone']['enabled'] = True
        with self.assertRaisesRegex(ValueError, 'disable the bus'):
            generate.validate(self.settings)
        self.settings['bus']['enabled'] = False
        settings, rules = generate.validate(self.settings)
        self.assertIn('standalone_enabled = true', generate.emit(settings, rules))
        for role in ('child', 'follower'):
            with self.assertRaisesRegex(ValueError, 'parent role'):
                generate.validate(self.settings, role, 1)
        for value in (0, 99, 60001, True):
            self.settings['standalone']['ble_dwell_ms'] = value
            with self.assertRaises(ValueError):
                generate.validate(self.settings)

    def test_legacy_config_defaults_to_distributed(self):
        del self.settings['standalone']
        settings, _ = generate.validate(self.settings)
        self.assertFalse(settings['standalone']['enabled'])

    def test_standalone_example(self):
        settings, _ = generate.validate(generate.load(ROOT/'config/settings.standalone.example.json'))
        self.assertTrue(settings['standalone']['enabled'])
        self.assertFalse(settings['bus']['enabled'])

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
    def test_universal_discovery_budget(self):
        self.settings['node']['role'] = 'universal'
        self.settings['bus'].update(baud=9600, poll_timeout_ms=600, last_node_id=16,
                                    membership_lease_ms=20000, assignment_lease_ms=30000)
        with self.assertRaisesRegex(ValueError, 'two worst-case bus sweeps'):
            generate.validate(self.settings)
        self.settings['bus'].update(membership_lease_ms=60000, assignment_lease_ms=60000)
        # At 9600 baud the fully populated discovery schedule cannot fit valid leases.
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
        self.settings['bus'].update(last_node_id=2, membership_lease_ms=10000,
                                    assignment_lease_ms=20000)
        generate.validate(self.settings)

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
