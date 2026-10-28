"""Exercise exported files as downstream spreadsheet and mapping tools would."""
import contextlib
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('export', ROOT / 'tools/export_observations.py')
export = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(export)


def observation(**changes):
    return dict(dict(schema_version=1, node=2, boot=4, receiver_boot=8, sequence=6,
                     radio='wifi', address='00:11:22:33:44:55', address_type=0,
                     channel=36, capture_ms=2500, utc_ms=0, rssi_dbm=-52,
                     ssid='=IMPORTXML("example")', rule_id='test-rule',
                     receiver_fix={'latitude': 42.1, 'longitude': -83.2, 'fix_at_ms': 2490},
                     peak_receiver_fix={'latitude': 43, 'longitude': -84, 'fix_at_ms': 2000}),
                **changes)


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.log = self.root / 'WK00000001.LOG'
        self.output = self.root / 'export'

    def run_export(self, rows, kind='geojson', extra=()):
        self.log.write_text(''.join(json.dumps(row) + '\n' for row in rows), encoding='utf-8')
        return export.main([str(self.log), '--format', kind, '-o', str(self.output), *extra])

    def test_json_formats_preserve_complete_detections(self):
        drone = observation(event='drone_detection', message_type=1,
                            data={'latitude': 42, 'longitude': -83},
                            authenticated=False, rule_id=None, name='雪')
        for kind in ('json', 'jsonl'):
            with self.subTest(kind=kind):
                self.assertEqual(self.run_export([{'event': 'status'}, drone], kind,
                                                 ['--matches-only']), 0)
                text = self.output.read_text()
                rows = json.loads(text) if kind == 'json' else [json.loads(line) for line in text.splitlines()]
                self.assertEqual(len(rows), 1)
                for key, value in drone.items():
                    self.assertEqual(rows[0][key], value)
                self.assertEqual(rows[0]['source_line'], 2)
                self.assertEqual(self.run_export([], kind), 0)
                self.assertEqual(self.output.read_text(), '[]\n' if kind == 'json' else '')

    def test_probe_evidence_exports(self):
        row = observation(wifi_subtype=4, wifi_payload_format='probe_ie_v1',
                          wifi_ie_hex='000001028284dd0412345601', wifi_remote_id_hex='')
        for kind in ('csv', 'tsv', 'json', 'jsonl', 'geojson'):
            with self.subTest(kind=kind):
                self.assertEqual(self.run_export([row],kind),0)
                if kind in ('csv','tsv'):
                    with self.output.open() as stream:
                        result = list(csv.DictReader(stream,delimiter=',' if kind=='csv' else '\t'))[0]
                else:
                    data = json.loads(self.output.read_text())
                    result = data['features'][0]['properties'] if kind=='geojson' else data[0] if kind=='json' else data
                self.assertEqual(str(result['wifi_subtype']),'4')
                self.assertEqual(result['wifi_payload_format'],'probe_ie_v1')
                self.assertEqual(result['wifi_ie_hex'],row['wifi_ie_hex'])

    def test_infers_extensions_and_defaults_stdout_to_json(self):
        self.log.write_text(json.dumps(observation()))
        for kind in ('json', 'jsonl', 'csv', 'tsv', 'geojson'):
            target = self.root / ('detections.' + kind.upper())
            self.assertEqual(export.main([str(self.log), '-o', str(target)]), 0)
            self.assertTrue(target.read_text())
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(export.main([str(self.log)]), 0)
        self.assertEqual(len(json.loads(output.getvalue())), 1)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            export.main([str(self.log), '-o', str(self.output)])

    def test_tabular_drone_evidence(self):
        drone = observation(event='drone_detection', data={'description': 'a,\tb\n雪'},
                            protocol='remote_id', authenticated=False, raw_hex='abcd',
                            transport='wifi_nan', remote_id_transport='wifi_nan')
        for kind, delimiter in (('csv', ','), ('tsv', '\t')):
            self.assertEqual(self.run_export([drone], kind), 0)
            with self.output.open(newline='') as stream:
                row, = csv.DictReader(stream, delimiter=delimiter)
            self.assertEqual(json.loads(row['data']), drone['data'])
            self.assertEqual(row['protocol'], 'remote_id')
            self.assertEqual(row['raw_hex'], 'abcd')
            self.assertEqual(row['transport'], 'wifi_nan')
            self.assertEqual(row['remote_id_transport'], 'wifi_nan')

    def test_drone_map_distinguishes_aircraft_operator_and_receiver(self):
        aircraft = observation(event='drone_detection', message_type=1,
                               data={'latitude': 40, 'longitude': -80})
        operator = observation(event='drone_detection', message_type=4,
                               data={'operator_position': {'latitude': 41, 'longitude': -81}})
        unknown = observation(event='drone_detection', message_type=1,
                              data={'latitude': None, 'longitude': None})
        for location, index, point in (('aircraft', 0, [-80, 40]), ('operator', 1, [-81, 41])):
            self.assertEqual(self.run_export([aircraft, operator, unknown, observation()],
                                             extra=['--location', location]), 0)
            features = json.loads(self.output.read_text())['features']
            for i, feature in enumerate(features):
                self.assertEqual(feature['properties']['location_role'], location)
                self.assertEqual(feature['geometry'], {'type': 'Point', 'coordinates': point}
                                 if i == index else None)
        bad = observation(event='drone_detection', message_type=1,
                          data={'latitude': 100, 'longitude': 0})
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(self.run_export([bad, aircraft], extra=['--location', 'aircraft',
                                                                    '--skip-invalid']), 0)
        self.assertIn('WARNING: skipped', errors.getvalue())
        self.assertEqual(len(json.loads(self.output.read_text())['features']), 1)

    def test_geojson_preserves_rule_evidence_and_unknown_locations(self):
        self.assertEqual(self.run_export([observation(), observation(rule_id='other'),
                                         observation(receiver_fix=None)]), 0)
        result = json.loads(self.output.read_text())
        self.assertEqual(result['type'], 'FeatureCollection')
        self.assertEqual(len(result['features']), 3)
        first, second, unknown = result['features']
        self.assertEqual(first['geometry']['coordinates'], [-83.2, 42.1])
        self.assertEqual(first['properties']['location_role'], 'receiver')
        self.assertEqual(first['properties']['source_line'], 1)
        self.assertEqual(second['properties']['rule_id'], 'other')
        self.assertIsNone(unknown['geometry'])

    def test_peak_map_and_following_alert_without_peak(self):
        alert = dict(event='following_alert', radio='ble', address='00:11:22:33:44:55',
                     displacement_m=250, receiver_fix=None)
        self.assertEqual(self.run_export([observation(), alert], extra=['--location', 'peak']), 0)
        features = json.loads(self.output.read_text())['features']
        self.assertEqual(features[0]['geometry']['coordinates'], [-84, 43])
        self.assertIsNone(features[1]['geometry'])

    def test_csv_is_parseable_and_spreadsheet_text_is_inert(self):
        self.assertEqual(self.run_export([observation(name='a,"b"\nnext')], 'csv'), 0)
        with self.output.open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['ssid'], "'=IMPORTXML(\"example\")")
        self.assertEqual(rows[0]['name'], 'a,"b"\nnext')
        self.assertEqual(rows[0]['rssi_dbm'], '-52')
        self.assertEqual(rows[0]['receiver_fix_at_ms'], '2490')
        self.assertEqual(rows[0]['utc'], '1970-01-01T00:00:00.000Z')

    def test_filters_status_and_nonmatches(self):
        rows = [observation(rule_id=None), {'event': 'status'}, observation()]
        self.assertEqual(self.run_export(rows, extra=['--matches-only']), 0)
        self.assertEqual(len(json.loads(self.output.read_text())['features']), 1)

    def test_malformed_input_does_not_replace_existing_output(self):
        self.output.write_text('previous export')
        self.log.write_text(json.dumps(observation()) + '\n{"schema_version":')
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(export.main([str(self.log), '--format', 'geojson', '-o', str(self.output)]), 1)
        self.assertIn(':2:', errors.getvalue())
        self.assertEqual(self.output.read_text(), 'previous export')
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(export.main([str(self.log), '--format', 'geojson', '-o', str(self.output), '--skip-invalid']), 0)
        self.assertIn('WARNING: skipped', errors.getvalue())
        self.assertEqual(len(json.loads(self.output.read_text())['features']), 1)
        self.assertFalse(list(self.root.glob('.wiknow-export-*')))

    def test_rejects_invalid_coordinates_versions_and_nonfinite_values(self):
        invalid = [observation(receiver_fix={'latitude': 91, 'longitude': 0}),
                   observation(receiver_fix={'latitude': True, 'longitude': 0}),
                   observation(schema_version=2), observation(schema_version=True),
                   observation(rssi_dbm=float('nan'))]
        for row in invalid:
            with self.subTest(row=row), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(self.run_export([row]), 1)

    def test_stdout_multiple_inputs_and_null_utc(self):
        self.log.write_text(json.dumps(observation(utc_ms=None)) + '\n')
        another = self.root / 'second.log'
        another.write_text(json.dumps(observation(sequence=7)) + '\n')
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(export.main([str(self.log), str(another), '--format', 'csv']), 0)
        rows = list(csv.DictReader(io.StringIO(output.getvalue())))
        self.assertEqual(rows[0]['utc'], '')
        self.assertEqual(rows[1]['sequence'], '7')

    def test_refuses_to_overwrite_source_log(self):
        self.log.write_text(json.dumps(observation()))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            export.main([str(self.log), '--format', 'csv', '-o', str(self.log)])

    def test_skip_invalid_covers_numeric_overflow_and_bad_utc(self):
        for kind in ('csv', 'geojson'):
            with self.subTest(kind=kind):
                self.log.write_text('\n'.join([
                    json.dumps(observation()).replace('-52', '1e999'),
                    json.dumps(observation(utc_ms='tomorrow')),
                    json.dumps(observation(peak_utc_ms=10**100)),
                    json.dumps(observation()),
                ]))
                with contextlib.redirect_stderr(io.StringIO()) as errors:
                    self.assertEqual(export.main([str(self.log), '--format', kind,
                                                  '-o', str(self.output), '--skip-invalid']), 0)
                self.assertEqual(errors.getvalue().count('WARNING: skipped'), 3)
                if kind == 'csv':
                    with self.output.open(newline='') as stream:
                        self.assertEqual(len(list(csv.DictReader(stream))), 1)
                else:
                    self.assertEqual(len(json.loads(self.output.read_text())['features']), 1)


if __name__ == '__main__':
    unittest.main()
