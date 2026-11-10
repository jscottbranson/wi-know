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
    def test_gps_without_backup_battery_or_tx(self):
        for name in ('settings.standalone.example.json', 'settings.parent.example.json'):
            source = generate.load(ROOT/'config'/name)
            source['gps']['backup_battery_present'] = False
            with self.subTest(config=name):
                settings, rules = generate.validate(source, board_override='n32r8')
                self.assertTrue(settings['gps']['enabled'])
                self.assertEqual(settings['gps']['tx_pin'], 8)
                receive_only = copy.deepcopy(settings)
                receive_only['board'] = {'model': 'custom'}
                receive_only['capture_switch']['enabled'] = False
                receive_only['gps']['tx_pin'] = -1
                generate.validate(receive_only)
                self.assertIn('gps_backup_battery_present = false', generate.emit(settings, rules))

    def test_gps_models(self):
        for model in ('Grove GPS Air530', 'Grove GPS Air530Z'):
            self.settings['gps']['model'] = model
            generate.validate(self.settings)
        self.settings['gps']['model'] = 'unsupported'
        with self.assertRaisesRegex(ValueError, 'unsupported GPS model'):
            generate.validate(self.settings)

    def test_removed_amber_config_and_gps_tx_conflict(self):
        source = generate.load(ROOT/'config/settings.parent.example.json')
        source['signal_trend']['steady_pin'] = 8
        with self.assertRaises(ValueError):
            generate.validate(source)
        del source['signal_trend']['steady_pin']
        source['board'] = {'model': 'custom'}
        source['capture_switch']['enabled'] = False
        source['gps']['tx_pin'] = source['signal_trend']['increasing_pin']
        with self.assertRaisesRegex(ValueError, 'used by'):
            generate.validate(source)

    def test_capture_switch_validation(self):
        source = generate.load(ROOT/'config/settings.standalone.example.json')
        source['board'] = {'model': 'xiao'}
        source['capture_switch'] = {'enabled': True, 'gpio_pin': 1,
                                    'internal_pulldown': True, 'debounce_ms': 50}
        with self.assertRaisesRegex(ValueError, 'used by'):
            generate.validate(source)
        source['signal_trend']['increasing_pin'] = -1
        valid, rules = generate.validate(source)
        self.assertEqual(valid['signal_trend']['increasing_pin'], -1)
        self.assertIn('capture_switch_gpio_pin = 1', generate.emit(valid, rules))
        for board, pin in [('xiao', 2), ('xiao', 7), ('xiao', 25), ('xiao', 26),
                           ('xiao', 13), ('xiao', 15), ('xiao', 28), ('n32r8', 6),
                           ('n32r8', 12), ('n32r8', 15), ('n32r8', 27), ('custom', 0)]:
            bad = copy.deepcopy(source)
            bad['board']['model'] = board
            bad['capture_switch']['gpio_pin'] = pin
            with self.subTest(board=board, pin=pin), self.assertRaises(ValueError):
                generate.validate(bad)
        for key, value in [('debounce_ms', 0), ('gpio_pin', -2), ('internal_pulldown', 1)]:
            bad = copy.deepcopy(source)
            bad['capture_switch'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                generate.validate(bad)
        for role in ('child', 'follower', 'universal'):
            with self.subTest(role=role), self.assertRaises(ValueError):
                generate.validate(source, role_override=role)
        defaults, _ = generate.validate(self.settings)
        self.assertFalse(defaults['capture_switch']['enabled'])
        self.assertEqual(defaults['capture_switch']['gpio_pin'], -1)

    def test_capture_examples_keep_two_leds_and_gps_tx(self):
        for variant, pin, gps_tx in [('parent', 26, 8), ('standalone', 4, 0)]:
            source = generate.load(ROOT/f'config/settings.{variant}.example.json')
            settings, _ = generate.validate(source, board_override=source['board']['model'])
            self.assertEqual(settings['capture_switch']['gpio_pin'], pin)
            self.assertTrue(settings['capture_switch']['internal_pulldown'])
            self.assertEqual(settings['gps']['tx_pin'], gps_tx)
            for key in ('increasing_pin', 'decreasing_pin'):
                self.assertGreaterEqual(settings['signal_trend'][key], 0)
            # A configurable input must still not overlap a fitted peripheral.
            source['capture_switch']['gpio_pin'] = gps_tx
            with self.assertRaisesRegex(ValueError, 'used by'):
                generate.validate(source)

    def test_all_standalone_configs_select_capture_pin_by_board(self):
        for variant in ('standalone',):
            source = generate.load(ROOT/f'config/settings.{variant}.example.json')
            for board, pin in [('xiao', 4), ('n32r8', 26)]:
                with self.subTest(variant=variant, board=board):
                    settings, rules = generate.validate(source, board_override=board)
                    self.assertTrue(settings['standalone']['enabled'])
                    self.assertFalse(settings['bus']['enabled'])
                    self.assertTrue(settings['capture_switch']['enabled'])
                    self.assertEqual(settings['capture_switch']['gpio_pin'], pin)
                    self.assertIn(f'capture_switch_gpio_pin = {pin}', generate.emit(settings, rules))
            self.assertEqual(source['capture_switch']['gpio_pin'], -1)
            source['capture_switch']['enabled'] = False
            settings, _ = generate.validate(source, board_override='n32r8')
            self.assertFalse(settings['capture_switch']['enabled'])

    def test_board_profiles_match_documented_wiring_and_preserve_behavior(self):
        source = generate.load(ROOT/'config/settings.standalone.example.json')
        original = copy.deepcopy(source)
        for board, reference in [('xiao', 'standalone'), ('n32r8', 'parent')]:
            expected = generate.load(ROOT/f'config/settings.{reference}.example.json')
            settings, rules = generate.validate(source, board_override=board)
            self.assertEqual(settings['board']['model'], board)
            for section in ('display', 'buzzer', 'signal_trend', 'gps', 'sd'):
                self.assertEqual(settings[section], expected[section])
            self.assertTrue(settings['standalone']['enabled'])
            self.assertFalse(settings['bus']['enabled'])
            self.assertEqual(settings['detection'], source['detection'])
            self.assertIn(f'board_model = "{board}"', generate.emit(settings, rules))
        self.assertEqual(source, original)

    def test_board_profile_config_override_and_custom_escape_hatch(self):
        self.settings['board'] = {'model': 'n32r8'}
        settings, _ = generate.validate(self.settings)
        self.assertEqual(settings['display']['sda_pin'], 4)
        self.assertFalse(settings['display']['enabled'])
        settings, _ = generate.validate(self.settings, board_override='custom')
        self.assertEqual(settings['display']['sda_pin'], -1)
        with self.assertRaisesRegex(ValueError, 'invalid board'):
            generate.validate(self.settings, board_override='unknown')
        with self.assertRaisesRegex(ValueError, 'manual role'):
            generate.validate(self.settings, 'universal')

    def test_board_mapping_is_checked_for_conflicts_after_resolution(self):
        settings = generate.load(ROOT/'config/settings.parent.example.json')
        with self.assertRaisesRegex(ValueError, 'GPIO .* used'):
            generate.validate(settings, board_override='xiao')
        settings, _ = generate.validate(self.settings, 'child', board_override='xiao')
        self.assertEqual(settings['bus']['rx_pin'], 12)

    def test_sd_formatting_defaults_on_with_explicit_boolean_opt_out(self):
        settings, rules = generate.validate(self.settings)
        self.assertTrue(settings['sd']['format_if_mount_failed'])
        self.assertIn('sd_format_if_mount_failed = true', generate.emit(settings, rules))
        self.settings['sd']['format_if_mount_failed'] = False
        settings, rules = generate.validate(self.settings)
        self.assertIn('sd_format_if_mount_failed = false', generate.emit(settings, rules))
        for invalid in (1, 'true', None):
            self.settings['sd']['format_if_mount_failed'] = invalid
            with self.assertRaises(ValueError):
                generate.validate(self.settings)

    def test_universal_profiles_and_strap_conflicts(self):
        for name in ('settings.json',):
            settings, rules = generate.validate(generate.load(ROOT/'config'/name))
            self.assertEqual(settings['node']['role'], 'universal')
            header = generate.emit(settings, rules)
            self.assertIn('universal_enabled = true', header)
            self.assertIn('universal_parent_rx_pin = 3', header)
            self.assertIn('universal_worker_rx_pin = 12', header)
            self.assertIn('universal_strap_pin = 10', header)
        settings = generate.load(ROOT/'config/settings.json')
        settings['gps']['enabled'] = True
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

    def test_following_short_gap_without_scoring(self):
        self.settings['following_alert'].update(enabled=False, distance_m=100, delay_ms=0, max_gap_ms=1, ignore_devices=[])
        settings, _ = generate.validate(self.settings)
        self.assertFalse(settings['following_alert']['scoring_enabled'])

    def test_following_scoring(self):
        self.settings['gps'].update(enabled=True, rx_pin=5)
        self.settings['following_alert'].update(enabled=True, scoring_enabled=True)
        settings, rules = generate.validate(self.settings)
        self.assertIn('following_alert_score_threshold', generate.emit(settings, rules))
        for field, value in [('min_duration_ms', 0), ('min_hits', 1), ('hit_interval_ms', 0),
                             ('score_threshold', True), ('score_decay_ms', 0), ('gps_jitter_m', 0),
                             ('max_speed_mps', 501), ('max_fix_age_ms', -1), ('enabled', False)]:
            invalid = copy.deepcopy(self.settings)
            invalid['following_alert'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                generate.validate(invalid)
        for path in (ROOT / 'config').glob('settings*.json'):
            self.assertFalse(generate.load(path)['following_alert']['scoring_enabled'])

    def test_standalone_wiring_uses_all_header_pins(self):
        settings, rules = generate.validate(generate.load(ROOT/'config/settings.standalone.example.json'))
        self.assertEqual(settings['node'], {'role': 'parent'})
        self.assertTrue(settings['standalone']['enabled'])
        self.assertFalse(settings['bus']['enabled'])
        header = generate.emit(settings, rules)
        assignments = {
            'display': {'sda_pin': 23, 'scl_pin': 24},
            'gps': {'rx_pin': 12, 'tx_pin': 0},
            'sd': {'sck_pin': 8, 'mosi_pin': 10, 'miso_pin': 9, 'cs_pin': 11},
            'buzzer': {'pin': 7},
            'signal_trend': {'increasing_pin': 1, 'decreasing_pin': 25},
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
        self.assertEqual(settings['gps']['tx_pin'], 0)
        self.assertEqual(settings['gps']['baud'], 9600)
        self.assertEqual(settings['sd']['clock_khz'], 4000)

    def test_standalone_priority_peripheral_combinations(self):
        example = generate.load(ROOT/'config/settings.standalone.example.json')
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
        example = generate.load(ROOT/'config/settings.standalone.example.json')
        example['board'] = {'model': 'custom'}
        example['capture_switch']['enabled'] = False
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
        example = generate.load(ROOT/'config/settings.standalone.example.json')
        example['board'] = {'model': 'custom'}
        example['capture_switch']['enabled'] = False
        keys = ('increasing_pin', 'decreasing_pin')
        for mask in range(4):
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
        settings['signal_trend']['increasing_pin'] = -1
        settings['gps'].update(enabled=True, rx_pin=1)
        generate.validate(settings)
        settings['signal_trend']['increasing_pin'] = 1
        with self.assertRaisesRegex(ValueError, 'GPIO 1 used'):
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
        self.assertIn('discovery_enabled = false', generate.emit(settings, rules))
        for role in ('child', 'follower'):
            with self.assertRaisesRegex(ValueError, 'parent role'):
                generate.validate(self.settings, role)
        for value in (0, 99, 60001, True):
            self.settings['standalone']['ble_dwell_ms'] = value
            with self.assertRaises(ValueError):
                generate.validate(self.settings)

    def test_missing_settings_are_rejected(self):
        for section, fields in self.settings.items():
            missing = copy.deepcopy(self.settings)
            del missing[section]
            with self.subTest(section=section), self.assertRaises(ValueError):
                generate.validate(missing)
            if isinstance(fields, dict):
                for field in fields:
                    missing = copy.deepcopy(self.settings)
                    del missing[section][field]
                    with self.subTest(section=section, field=field), self.assertRaises(ValueError):
                        generate.validate(missing)

    def test_standalone_example(self):
        settings, _ = generate.validate(generate.load(ROOT/'config/settings.standalone.example.json'))
        self.assertTrue(settings['standalone']['enabled'])
        self.assertFalse(settings['bus']['enabled'])

    def test_expanded_predicate_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = str(base)
            document = generate.load(ROOT/'rules/examples/lab.json')
            rule = document['rules'][0]
            cases = [
                ('ble', 'service_uuid16_range', '3100-3500', True),
                ('ble', 'service_uuid16_range', 'FEED-FEED', True),
                ('ble', 'service_uuid16_range', '3500-3100', False),
                ('ble', 'service_uuid16_range', '3100:3500', False),
                ('ble', 'service_uuid16_range', '310-3500', False),
                ('wifi', 'service_uuid16_range', '3100-3500', False),
                ('wifi', 'wifi_ie_signature', '2,12,221:506F9A16030103', True),
                ('wifi', 'wifi_ie_signature', '0,2', False),
                ('wifi', 'wifi_ie_signature', '221:112233', False),
                ('wifi', 'wifi_ie_signature', '221:001122334455667788', False),
                ('wifi', 'wifi_ie_signature', '221', False),
                ('wifi', 'wifi_ie_signature', '256', False),
                ('wifi', 'wifi_ie_signature', '2,', False),
                ('ble', 'wifi_ie_signature', '2,12', False),
                ('ble', 'name_contains_ci', 'Ray-Ban', True),
                ('ble', 'name_exact', 'HC-05', True),
                ('wifi', 'name_exact', 'HC-05', False),
                ('wifi', 'ssid_contains_ci', 'Camera', True),
                ('wifi', 'ssid_ring_setup', 'true', True),
                ('wifi', 'ssid_ring_setup', 'false', False),
                ('wifi', 'ssid_ring_setup', 'Ring-', False),
                ('ble', 'ssid_ring_setup', 'true', False),
                ('ble', 'ssid_contains_ci', 'Camera', False),
                ('ble', 'name_contains_ci', 'x'*33, False),
                ('wifi', 'ssid_contains_ci', 'bad\nname', False),
            ]
            for radio, field, value, valid in cases:
                with self.subTest(radio=radio, field=field, value=value):
                    rule['radio'] = radio
                    rule['predicates'] = [{'field': field, 'value': value}]
                    (base/'rule.json').write_text(json.dumps(document))
                    if valid:
                        _, rules = generate.validate(self.settings)
                        if field in ('service_uuid16_range', 'wifi_ie_signature'):
                            self.assertEqual(rules[0]['predicates'][0]['value'], value.lower())
                    else:
                        with self.assertRaises(ValueError):
                            generate.validate(self.settings)

    def test_detection_category_defaults_and_opt_out(self):
        disabled = {'ble_trackers', 'unifi_cameras'}
        for path in sorted((ROOT/'config').glob('settings*.json')):
            with self.subTest(profile=path.name):
                original = generate.load(path)
                settings, rules = generate.validate(original)
                self.assertEqual(settings['detection']['disabled_categories'], [])
                self.assertTrue(rules)
                self.assertTrue(all(r['enabled'] for r in rules))
                self.assertTrue({'cameras', *disabled} <= {r['category'] for r in rules})
                self.assertTrue(all(len(r['source']['revision']) >= 7 for r in rules))
                original['detection']['disabled_categories'] = sorted(disabled)
                original['detection']['enabled_categories'] = [
                    category for category in original['detection']['enabled_categories']
                    if category not in disabled]
                _, opted_out = generate.validate(original)
                self.assertEqual({r['category'] for r in opted_out if not r['enabled']}, disabled)
                self.assertTrue(all(r['enabled'] for r in opted_out if r['category'] not in disabled))
        self.settings['detection']['disabled_categories'].append('ble_trackers')
        with self.assertRaisesRegex(ValueError, 'both enabled and disabled'):
            generate.validate(self.settings)

    def test_disabled_categories_validation(self):
        for invalid in (['a', 'a'], ['Bad Category'], [1], 'ble_trackers'):
            self.settings['detection']['disabled_categories'] = invalid
            with self.assertRaises(ValueError):
                generate.validate(self.settings)

    def test_defaults(self):
        settings, rules = generate.validate(self.settings)
        self.assertEqual(settings['node']['role'], 'parent')
        self.assertTrue(rules)
        self.assertTrue({'axon', 'flock', 'ring', 'alpr', 'body_cameras'} <=
                        {rule['category'] for rule in rules})

    def test_shipped_profiles_enable_surveillance_pack(self):
        categories = {'axon', 'flock', 'ring', 'alpr', 'body_cameras'}
        for path in sorted((ROOT/'config').glob('settings*.json')):
            with self.subTest(profile=path.name):
                settings, rules = generate.validate(generate.load(path))
                self.assertTrue(categories <= set(settings['detection']['enabled_categories']))
                self.assertTrue(categories <= {rule['category'] for rule in rules
                                               if rule['enabled']})

    def test_all_roles_use_parent_assigned_ids(self):
        self.settings['bus'].update(first_node_id=3, last_node_id=5)
        for role in ('parent', 'child', 'follower', 'universal'):
            settings, rules = generate.validate(self.settings, role)
            self.assertIn('discovery_enabled = true', generate.emit(settings, rules))
            invalid = copy.deepcopy(self.settings)
            invalid['node']['id'] = 0
            with self.assertRaisesRegex(ValueError, 'unknown keys'):
                generate.validate(invalid, role)

    def test_unwired_peripheral(self):
        self.settings['gps']['enabled'] = True
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
    def test_pin_conflict(self):
        self.settings['gps'].update(enabled=True, rx_pin=5)
        self.settings['buzzer'].update(enabled=True, pin=5)
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
    def test_dfs_and_channel_165(self):
        channels = list(range(1, 12)) + list(range(36, 65, 4)) + list(range(100, 145, 4)) + list(range(149, 166, 4))
        self.settings['wifi']['channels'] = channels
        validated, _ = generate.validate(self.settings)
        self.assertEqual(validated['wifi']['channels'], channels)
        self.assertEqual(len(channels), 36)

    def test_bad_channels(self):
        for channels in ([1, 1], [12], [68], [96], [148], [169], [], [True], [[]], [{}]):
            settings = copy.deepcopy(self.settings)
            settings['wifi']['channels'] = channels
            with self.assertRaises(ValueError):
                generate.validate(settings)
    def test_remote_id_predicates_and_examples(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = '.'
            self.settings['detection']['enabled_categories'] = ['drones']
            document = generate.load(ROOT/'rules/examples/remote-id.json')
            for rule in document['rules']:
                rule['enabled'] = True
            path = base/'remote-id.json'
            path.write_text(json.dumps(document))
            settings, rules = generate.validate(self.settings, base=base)
            self.assertEqual(len(rules), 4)
            self.assertTrue(all(rule['enabled'] for rule in rules))
            header = generate.emit(settings, rules)
            for field in ('rid_valid', 'rid_basic_id', 'rid_basic_id_type'):
                self.assertIn('Field::'+field, header)
            self.assertEqual(rules[1]['source'], document['rules'][1]['source'])
            self.assertEqual(rules[1]['predicates'], document['rules'][1]['predicates'])
            self.settings['detection']['enabled_categories'] = ['lab']
            _, rules = generate.validate(self.settings, base=base)
            self.assertFalse(any(rule['enabled'] for rule in rules))
            document['rules'] = document['rules'][:1]
            valid = [('rid_valid', 'true'), ('rid_position_valid', 'true'),
                     ('rid_basic_id', '12345678901234567890'),
                     *[('rid_basic_id_type', str(i)) for i in range(1, 5)],
                     *[('rid_message_type', name) for name in
                       ('basic_id', 'location', 'authentication', 'self_id', 'system', 'operator_id')]]
            invalid = [('rid_valid', 'false'), ('rid_valid', True),
                       ('rid_position_valid', 'false'), ('rid_message_type', '6'),
                       ('rid_message_type', 'pack'), ('rid_basic_id_type', '0'),
                       ('rid_basic_id_type', '5'), ('rid_basic_id_type', '01'),
                       ('rid_basic_id', 'a'*21), ('rid_basic_id', ''),
                       ('rid_basic_id', 'test\x00id'), ('rid_basic_id', 'test\nid'),
                       ('rid_basic_id', 'é')]
            for radio in ('ble', 'wifi'):
                document['rules'][0]['radio'] = radio
                for pairs, accepted in ((valid, True), (invalid, False)):
                    for field, value in pairs:
                        with self.subTest(radio=radio, field=field, value=value):
                            document['rules'][0]['predicates'] = [{'field': field, 'value': value}]
                            path.write_text(json.dumps(document))
                            if accepted:
                                settings, rules = generate.validate(self.settings, base=base)
                                self.assertIn('Field::'+field, generate.emit(settings, rules))
                            else:
                                with self.assertRaises(ValueError):
                                    generate.validate(self.settings, base=base)

    def test_ble_payload_contains(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = str(base)
            doc = generate.load(ROOT/'rules/examples/lab.json')
            doc['rules'] = doc['rules'][:1]
            rule = doc['rules'][0]
            rule['radio'] = 'ble'
            rule['predicates'] = [{'field': 'ble_payload_contains', 'value': '00FF80'}]
            path = base/'payload.json'
            for value in ('00', 'FF'*32, '425743444556494345', '00FF80'):
                with self.subTest(value=value):
                    rule['predicates'][0]['value'] = value
                    path.write_text(json.dumps(doc))
                    settings, rules = generate.validate(self.settings)
                    self.assertEqual(rules[0]['predicates'][0]['value'], value.lower())
                    self.assertIn('Field::ble_payload_contains,"'+value.lower()+'"',
                                  generate.emit(settings, rules))
            # Check actual output sidecar retains normalized binary predicate and attribution.
            settings_path = base/'settings.config'
            settings_path.write_text(json.dumps(self.settings))
            output = base/'settings.hpp'
            subprocess.run([sys.executable, str(ROOT/'tools/generate_config.py'),
                            '--config', str(settings_path), '--output', str(output)],
                           check=True, capture_output=True)
            manifest = json.loads(output.with_suffix('.rules.json').read_text())
            self.assertEqual(manifest['rules'][0]['predicates'], rules[0]['predicates'])
            self.assertEqual(manifest['rules'][0]['source'], rule['source'])
            output.with_suffix('.rules.json').unlink()
            for value in ('', '0', '123', 'ff'*33, 'GG', '0x00', '00:ff',
                          '00-ff', '00 ff', ' ff', 'ff\n', 'ＡＢ', 42, None):
                with self.subTest(value=value):
                    rule['predicates'][0]['value'] = value
                    path.write_text(json.dumps(doc))
                    with self.assertRaises(ValueError):
                        generate.validate(self.settings)
            rule['predicates'][0]['value'] = '00ff'
            rule['radio'] = 'wifi'
            path.write_text(json.dumps(doc))
            with self.assertRaisesRegex(ValueError, 'BLE predicate'):
                generate.validate(self.settings)

    def test_service_uuid128(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = '.'
            doc = generate.load(ROOT/'rules/examples/lab.json')
            doc['rules'] = doc['rules'][:1]
            rule = doc['rules'][0]
            rule['radio'] = 'ble'
            value = '00112233-4455-6677-8899-aabbccddeeff'
            rule['predicates'] = [{'field': 'service_uuid128', 'value': value.upper()}]
            path = base/'uuid.json'
            path.write_text(json.dumps(doc))
            settings, rules = generate.validate(self.settings, base=base)
            self.assertEqual(rules[0]['predicates'][0]['value'], value)
            self.assertEqual(rules[0]['source'], rule['source'])
            self.assertIn('Field::service_uuid128,"'+value+'"', generate.emit(settings, rules))
            # Exercise the actual provenance sidecar, not only in-memory rules.
            settings_path = base/'settings.config'
            cli_settings = copy.deepcopy(self.settings)
            cli_settings['detection']['rules_directory'] = str(base)
            settings_path.write_text(json.dumps(cli_settings))
            output = base/'settings.hpp'
            subprocess.run([sys.executable, str(ROOT/'tools/generate_config.py'),
                            '--config', str(settings_path), '--output', str(output)],
                           check=True, capture_output=True)
            manifest = json.loads(output.with_suffix('.rules.json').read_text())
            self.assertEqual(manifest['rules'][0]['predicates'], rules[0]['predicates'])
            self.assertEqual(manifest['rules'][0]['source'], rule['source'])
            output.with_suffix('.rules.json').unlink()
            for bad in (value.replace('-', ''), '{'+value+'}', 'urn:uuid:'+value,
                        value.replace('-', ':'), value+' ', value[:-1], value+'0',
                        value[:-1]+'g', '*', '', 123):
                with self.subTest(value=bad):
                    rule['predicates'][0]['value'] = bad
                    path.write_text(json.dumps(doc))
                    with self.assertRaises(ValueError):
                        generate.validate(self.settings, base=base)
            rule['predicates'][0]['value'] = value
            rule['radio'] = 'wifi'
            path.write_text(json.dumps(doc))
            with self.assertRaisesRegex(ValueError, 'BLE predicate'):
                generate.validate(self.settings, base=base)

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
        generate.validate(self.settings, 'child')
        self.assertEqual(self.settings, before)
        with self.assertRaises(ValueError):
            generate.validate(self.settings, 'invalid')
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

    def test_receiver_predicates(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = str(base)
            document = generate.load(ROOT/'rules/examples/lab.json')
            rule = document['rules'][0]
            rule.update(radio='wifi', confidence='possible')
            rule['actions']['follow'] = False
            for field, value, valid in [
                    ('wifi_receiver_mac', 'B4:1E:52:01:02:03', True),
                    ('wifi_receiver_mac', 'b4-1e-52-01-02-03', False),
                    ('wifi_receiver_mac', 'b4:1e:52', False),
                    ('wifi_receiver_oui', 'B4:1E:52', True),
                    ('wifi_receiver_oui', 'b4:1e', False),
                    ('wifi_receiver_oui', 'b4:1e:52:00', False),
                    ('wifi_subtype', 'data', True), ('wifi_subtype', 'qos_data', True)]:
                with self.subTest(field=field, value=value):
                    rule['predicates'] = [{'field': field, 'value': value}]
                    (base/'rule.json').write_text(json.dumps(document))
                    if valid:
                        settings, rules = generate.validate(self.settings)
                        self.assertIn('Field::'+field, generate.emit(settings, rules))
                    else:
                        with self.assertRaises(ValueError):
                            generate.validate(self.settings)
            rule['predicates'] = [{'field': 'wifi_receiver_mac', 'value': 'b4:1e:52:01:02:03'}]
            for field, value in [('radio', 'ble'), ('confidence', 'probable')]:
                old = rule[field]; rule[field] = value
                (base/'rule.json').write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    generate.validate(self.settings)
                rule[field] = old
            rule['actions']['follow'] = True
            (base/'rule.json').write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, 'cannot follow'):
                generate.validate(self.settings)
        settings = generate.load(ROOT/'config/settings.json')
        settings['detection']['enabled_categories'].remove('flock')
        _, rules = generate.validate(settings)
        receivers = [r for r in rules if r['id'].startswith('flock-receiver-')]
        self.assertEqual(len(receivers), 3)
        self.assertTrue(all(not r['enabled'] for r in receivers))

    def test_probe_predicates_and_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = str(base)
            document = generate.load(ROOT/'rules/examples/lab.json')
            rule = document['rules'][0]
            rule['radio'] = 'wifi'
            for field, value, valid in [
                ('wifi_subtype', 'probe_request', True), ('wifi_subtype', 'beacon', True),
                ('wifi_subtype', 'probe_response', True), ('wifi_subtype', 'nan_action', True),
                ('wifi_requested_ssid_prefix','Flock-',True),
                ('wifi_requested_ssid_prefix','é'*16,True),
                ('wifi_requested_ssid_prefix','é'*17,False),
                ('wifi_requested_ssid_prefix','a\x00b',False),
                ('wifi_requested_ssid_prefix','',False),
                ('wifi_requested_ssid_prefix','a'*33,False),
                ('wifi_subtype', '4', False), ('wifi_ssid_wildcard', 'true', True),
                ('wifi_ssid_wildcard', 'false', True), ('wifi_ssid_wildcard', 'yes', False),
                ('wifi_ie_fingerprint', '000001028284DD0412345601', True),
                ('wifi_ie_fingerprint', '000001fb' + '82'*251, True),
                ('wifi_ie_fingerprint', '01000100', True),
                ('wifi_ie_fingerprint', '00', False), ('wifi_ie_fingerprint', '0001', False),
                ('wifi_ie_fingerprint', '00000000', False), ('wifi_ie_fingerprint', 'dd03010203', False),
                ('wifi_ie_fingerprint', 'ff00', False), ('wifi_ie_fingerprint', '00:00', False),
                ('wifi_ie_fingerprint', '000', False), ('wifi_ie_fingerprint', 'gg', False),
                ('wifi_ie_fingerprint', '000001fb' + '82'*251 + '0100', False),
                ('wifi_ie_fingerprint', '0021' + '61'*33, False),
            ]:
                rule['predicates'] = [{'field': field, 'value': value}]
                (base/'rule.json').write_text(json.dumps(document))
                with self.subTest(field=field,value=value):
                    if valid:
                        settings, rules = generate.validate(self.settings)
                        self.assertIn('Field::'+field, generate.emit(settings,rules))
                        self.assertEqual(rules[0]['predicates'][0]['value'],
                                         value.lower() if field == 'wifi_ie_fingerprint' else value)
                    else:
                        with self.assertRaises(ValueError):
                            generate.validate(self.settings)
            rule['predicates'] = [{'field': 'wifi_subtype', 'value': 'probe_request'},
                                  {'field': 'wifi_ssid_wildcard', 'value': 'true'},
                                  {'field': 'mac', 'value': '02:11:22:33:44:55'},
                                  {'field': 'wifi_ie_fingerprint', 'value': '000001028284DD0412345601'}]
            (base/'rule.json').write_text(json.dumps(document))
            config = base/'config/settings.json'; config.parent.mkdir()
            config.write_text(json.dumps(self.settings))
            output = base/'generated/settings.hpp'
            result = subprocess.run([sys.executable, str(ROOT/'tools/generate_config.py'),
                                     '--config',str(config),'--output',str(output)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            manifest = generate.load(output.with_suffix('.rules.json'))['rules'][0]
            self.assertEqual(manifest['source'],rule['source'])
            self.assertEqual(manifest['predicates'][2], {'field':'mac','value':'02:11:22:33:44:55'})
            self.assertEqual(manifest['predicates'][-1]['value'],'000001028284dd0412345601')
            for pred in rule['predicates'][:2] + rule['predicates'][3:] + [
                    {'field':'wifi_requested_ssid_prefix','value':'Flock-'}]:
                rule['radio'] = 'ble'; rule['predicates'] = [pred]
                (base/'rule.json').write_text(json.dumps(document))
                with self.assertRaises(ValueError): generate.validate(self.settings)

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

    def test_mac_validation_canonicalization_and_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            base = pathlib.Path(folder)
            self.settings['detection']['rules_directory'] = str(base)
            document = generate.load(ROOT/'rules/examples/lab.json')
            document['rules'] = document['rules'][:1]
            rule = document['rules'][0]
            for radio in ('wifi', 'ble'):
                rule['radio'] = radio
                for value in ('02:ab:34:cd:56:ef', '02:AB:34:CD:56:EF',
                              '00:00:00:00:00:00', 'FF:FF:FF:FF:FF:FF'):
                    with self.subTest(radio=radio, value=value):
                        rule['predicates'] = [{'field': 'mac', 'value': value}]
                        (base/'rule.json').write_text(json.dumps(document))
                        settings, rules = generate.validate(self.settings, base=base)
                        self.assertEqual(rules[0]['predicates'][0]['value'], value.lower())
                        self.assertIn('Predicate{Field::mac,"'+value.lower()+'"}',
                                      generate.emit(settings, rules))
                for invalid in ('', '02:ab:34', '02:ab:34:cd:56', '02:ab:34:cd:56:ef:00',
                                '02:ab:34:cd:56:eg', '02:ab:34:cd:56:*', '02:ab:34:cd:56:ef/48',
                                '02ab34cd56ef', '02-ab-34-cd-56-ef', '02 ab 34 cd 56 ef',
                                ' 02:ab:34:cd:56:ef', '02:ab:34:cd:56:ef ', '0:2ab:34:cd:56:ef',
                                '02:ab:34:cd:56:ef\n', None, 123):
                    with self.subTest(radio=radio, invalid=invalid):
                        rule['predicates'] = [{'field': 'mac', 'value': invalid}]
                        (base/'rule.json').write_text(json.dumps(document))
                        with self.assertRaises(ValueError):
                            generate.validate(self.settings, base=base)
            rule['predicates'] = [{'field': 'mac', 'value': '02:AB:34:CD:56:EF'}]
            (base/'rule.json').write_text(json.dumps(document))
            # Put the config outside the globbed rules directory.
            settings_file = base/'config/settings.json'
            settings_file.parent.mkdir()
            settings_file.write_text(json.dumps(self.settings))
            output = base/'generated/settings.hpp'
            result = subprocess.run([sys.executable, str(ROOT/'tools/generate_config.py'),
                                     '--config', str(settings_file), '--output', str(output)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = generate.load(output.with_suffix('.rules.json'))['rules'][0]
            self.assertEqual(manifest['predicates'], [{'field': 'mac', 'value': '02:ab:34:cd:56:ef'}])
            for field in ('source', 'actions', 'priority', 'confidence'):
                self.assertEqual(manifest[field], rule[field])

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
    def test_board_builds_budget_discovery_exchanges(self):
        for variant in ('parent', 'child'):
            settings = generate.load(ROOT/f'config/settings.{variant}.example.json')
            settings['bus'].update(baud=9600, poll_timeout_ms=600, last_node_id=16,
                                   membership_lease_ms=20000, assignment_lease_ms=30000)
            with self.subTest(variant=variant), self.assertRaisesRegex(ValueError, 'two worst-case bus sweeps'):
                generate.validate(settings)

    def test_poll_timeout_covers_largest_reply_and_turnaround(self):
        # 448-byte framed session report at 115200 baud (39 ms) plus 20 ms turnaround.
        self.settings['bus'].update(baud=115200, poll_timeout_ms=59)
        generate.validate(self.settings)
        self.settings['bus']['poll_timeout_ms'] = 58
        with self.assertRaisesRegex(ValueError, 'poll timeout too short'):
            generate.validate(self.settings)

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
