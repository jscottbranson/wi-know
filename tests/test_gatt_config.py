import copy
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('generate', ROOT/'tools/generate_config.py')
generate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generate)

class GattConfigTests(unittest.TestCase):
    def setUp(self):
        self.settings = generate.load(ROOT/'config/settings.json')
    def test_defaults_and_invalid_targets(self):
        self.settings.pop('gatt')
        settings, _ = generate.validate(self.settings)
        self.assertFalse(settings['gatt']['enabled'])
        self.settings['gatt'] = settings['gatt']
        self.settings['gatt']['enabled'] = True
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
        target = {'address_type': 0, 'address': '12:34:56:78:9a:bc'}
        self.settings['gatt']['targets'] = [target]
        settings, rules = generate.validate(self.settings)
        self.assertIn('gatt_targets', generate.emit(settings, rules))
        for value in (-1, True, 2):
            target['address_type'] = value
            with self.assertRaises(ValueError):
                generate.validate(self.settings)
        target['address_type'] = 1
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
        target['address'] = 'd2:34:56:78:9a:bc'
        generate.validate(self.settings)
        for address in ('ff:ff:ff:ff:ff:ff', '00:00:00:00:00:00', 'bad', None):
            target['address'] = address
            with self.assertRaises(ValueError):
                generate.validate(self.settings)
        target['address'] = 'd2:34:56:78:9a:bc'
        self.settings['gatt']['targets'].append(copy.deepcopy(target))
        with self.assertRaises(ValueError):
            generate.validate(self.settings)
        self.settings['gatt']['targets'].pop()
        for key, value in (('cooldown_ms', 59999), ('timeout_ms', 10001)):
            old = self.settings['gatt'][key]
            self.settings['gatt'][key] = value
            with self.assertRaises(ValueError):
                generate.validate(self.settings)
            self.settings['gatt'][key] = old
    def test_rule_schema_and_disabled_category(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            doc = generate.load(ROOT/'rules/examples/lab.json')
            rule = doc['rules'][0]
            rule.update(radio='ble', enabled=True, match='all')
            rule['actions']['follow'] = False
            rule['predicates'] = [
                {'field': 'mac', 'value': '12:34:56:78:9a:bc'},
                {'field': 'gatt_service_uuid128', 'value': '00112233-4455-6677-8899-AABBCCDDEEFF'}]
            self.settings['detection']['rules_directory'] = str(folder)
            self.settings['detection']['enabled_categories'] = ['lab']
            def validate():
                (folder/'rules.json').write_text(json.dumps(doc))
                return generate.validate(self.settings)
            settings, rules = validate()
            self.assertTrue(rules[0]['enabled'])
            self.assertEqual(rules[0]['predicates'][1]['value'], '00112233-4455-6677-8899-aabbccddeeff')
            self.assertIn('gatt_service_uuid128', generate.emit(settings, rules))
            self.settings['detection']['enabled_categories'] = []
            _, rules = validate()
            self.assertFalse(rules[0]['enabled'])
            for key, value in (('radio', 'wifi'), ('match', 'any')):
                old = rule[key]
                rule[key] = value
                with self.assertRaises(ValueError):
                    validate()
                rule[key] = old
            rule['predicates'][0]['field'] = 'name_prefix'
            with self.assertRaises(ValueError):
                validate()
            rule['predicates'][0]['field'] = 'mac'
            rule['actions']['follow'] = True
            with self.assertRaises(ValueError):
                validate()
