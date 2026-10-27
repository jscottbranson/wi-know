#!/usr/bin/env python3
"""Export wi-know JSONL observations and alerts as CSV or GeoJSON (stdlib only)."""
import argparse
import csv
import datetime
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile


COLUMNS = (
    'source_file', 'source_line', 'event', 'node', 'boot', 'receiver_boot', 'sequence',
    'radio', 'address', 'address_type', 'channel', 'ssid', 'ssid_hex', 'name', 'rule_id', 'category',
    'confidence', 'rule_priority', 'capture_ms', 'utc_ms', 'utc', 'time_source',
    'timing_uncertainty_ms', 'gps_sync_age_ms', 'first_seen_ms', 'last_seen_ms',
    'rssi_dbm', 'receiver_latitude', 'receiver_longitude',
    'receiver_fix_at_ms', 'peak_dbm', 'peak_capture_ms', 'peak_utc_ms', 'peak_sequence',
    'peak_timing_uncertainty_ms', 'peak_receiver_latitude', 'peak_receiver_longitude', 'peak_receiver_fix_at_ms',
    'peak_changed', 'observations', 'displacement_m', 'delay_ms', 'ble_ad_hex',
    'location_role',
)


def reject_constant(value):
    raise ValueError(f'non-finite JSON number: {value}')


def finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        reject_constant(value)
    return result


def coordinate(fix):
    """Return receiver coordinates, or None for explicitly unknown locations."""
    if fix is None:
        return None
    if not isinstance(fix, dict):
        raise ValueError('receiver fix must be an object or null')
    lat, lon = fix.get('latitude'), fix.get('longitude')
    if not all(type(n) in (int, float) and math.isfinite(n) for n in (lat, lon)):
        raise ValueError('receiver fix needs finite latitude and longitude')
    if not -90 <= lat <= 90 or not -180 <= lon <= 180:
        raise ValueError('receiver fix coordinates out of range')
    return [lon, lat]


def records(paths, skip_invalid=False, matches_only=False):
    for path in paths:
        with path.open(encoding='utf-8') as source:
            for number, line in enumerate(source, 1):
                if not line.strip():
                    continue
                try:
                    record = json.loads(line, parse_constant=reject_constant, parse_float=finite_float)
                    if not isinstance(record, dict):
                        raise ValueError('expected a JSON object')
                    # Status records remain in the source log; they are not detections.
                    event = record.get('event', 'observation')
                    if event not in ('observation', 'following_alert'):
                        continue
                    version = record.get('schema_version', 1 if event == 'following_alert' else None)
                    if type(version) is not int or version != 1:
                        raise ValueError('unsupported or missing schema_version')
                    if record.get('radio') not in ('wifi', 'ble') or not isinstance(record.get('address'), str):
                        raise ValueError('detection needs radio and address')
                    if matches_only and event == 'observation' and not record.get('rule_id'):
                        continue
                    coordinate(record.get('receiver_fix'))
                    coordinate(record.get('peak_receiver_fix'))
                    utc_text(record.get('utc_ms'))
                    utc_text(record.get('peak_utc_ms'))
                    record = dict(record, source_file=str(path), source_line=number,
                                  event=event, location_role='receiver')
                    yield record
                except (ValueError, TypeError, OverflowError) as error:
                    message = f'{path}:{number}: {error}'
                    if not skip_invalid:
                        raise ValueError(message) from error
                    print(f'WARNING: skipped {message}', file=sys.stderr)


def utc_text(value):
    if value is None:
        return ''
    if type(value) is not int:
        raise ValueError('utc_ms must be an integer or null')
    epoch = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
    return (epoch + datetime.timedelta(milliseconds=value)).isoformat(timespec='milliseconds').replace('+00:00', 'Z')


def csv_value(value):
    # SSIDs and names are untrusted broadcast text. Keep spreadsheet imports inert.
    if isinstance(value, str) and value.lstrip().startswith(('=', '+', '-', '@')):
        return "'" + value
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=True, allow_nan=False)
    return value


def write_csv(rows, output):
    writer = csv.DictWriter(output, fieldnames=COLUMNS, extrasaction='ignore')
    writer.writeheader()
    for record in rows:
        row = dict(record, utc=utc_text(record.get('utc_ms')))
        for prefix, key in (('receiver', 'receiver_fix'), ('peak_receiver', 'peak_receiver_fix')):
            fix = record.get(key)
            if fix is not None:
                row.update({f'{prefix}_latitude': fix['latitude'],
                            f'{prefix}_longitude': fix['longitude'],
                            f'{prefix}_fix_at_ms': fix.get('fix_at_ms')})
        writer.writerow({key: csv_value(value) for key, value in row.items()})


def write_geojson(rows, output, location):
    output.write('{"type":"FeatureCollection","features":[')
    first = True
    for record in rows:
        key = 'receiver_fix' if location == 'capture' else 'peak_receiver_fix'
        point = coordinate(record.get(key))
        properties = dict(record, mapped_location=location)
        geometry = {'type': 'Point', 'coordinates': point} if point is not None else None
        feature = {'type': 'Feature', 'geometry': geometry, 'properties': properties}
        if not first:
            output.write(',')
        json.dump(feature, output, ensure_ascii=True, allow_nan=False, separators=(',', ':'))
        first = False
    output.write(']}\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path, help='JSONL log files in desired order')
    parser.add_argument('--format', choices=('csv', 'geojson'), required=True)
    parser.add_argument('--output', '-o', default='-', help='output path; default: stdout')
    parser.add_argument('--location', choices=('capture', 'peak'), default='capture',
                        help='receiver location to map in GeoJSON (default: capture)')
    parser.add_argument('--matches-only', action='store_true', help='only rule matches and following alerts')
    parser.add_argument('--skip-invalid', action='store_true', help='warn and skip invalid JSONL lines, including a truncated final line')
    args = parser.parse_args(argv)
    destination = Path(args.output) if args.output != '-' else None
    if destination and any(destination.resolve() == path.resolve() for path in args.inputs):
        parser.error('output must not replace an input log')
    temporary = None
    try:
        # Stage output so invalid input cannot replace a previous export or leave partial stdout.
        with tempfile.NamedTemporaryFile(mode='w+', encoding='utf-8', newline='',
                                         dir=destination.parent if destination else None,
                                         prefix='.wiknow-export-', delete=False) as output:
            temporary = Path(output.name)
            rows = records(args.inputs, args.skip_invalid, args.matches_only)
            if args.format == 'csv':
                write_csv(rows, output)
            else:
                write_geojson(rows, output, args.location)
            output.flush()
            if destination is None:
                output.seek(0)
                shutil.copyfileobj(output, sys.stdout)
        if destination:
            os.replace(temporary, destination)
        return 0
    except (OSError, ValueError, TypeError, OverflowError) as error:
        print(f'Export failed: {error}', file=sys.stderr)
        return 1
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    sys.exit(main())
