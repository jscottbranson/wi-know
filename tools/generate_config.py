#!/usr/bin/env python3
"""Validate JSON configuration and emit allocation-free C++ constants. Stdlib only."""
import argparse
import copy
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, low, high, name):
    require(type(value) is int and low <= value <= high, f'{name}: expected integer {low}..{high}')
    return value


def load(path):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f'duplicate JSON key: {key}')
            result[key] = value
        return result
    def constant(value):
        raise ValueError(f'nonstandard JSON value: {value}')
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=pairs,
                      parse_constant=constant)


def validate(settings, role_override=None, node_override=None, base=ROOT):
    # Overrides and category switches must not change the caller's saved settings.
    settings = copy.deepcopy(settings)
    if isinstance(settings.get('sd'), dict):
        settings['sd'].setdefault('format_if_mount_failed', True)
    # Optional section keeps existing schema-v1 configurations compatible.
    settings.setdefault('standalone', {'enabled': False, 'ble_dwell_ms': 120})
    settings.setdefault('universal', {'strap_pin': 10, 'parent_tx_pin': 11, 'parent_rx_pin': 3,
                                    'parent_rts_pin': 7, 'worker_tx_pin': 11,
                                    'worker_rx_pin': 12, 'worker_rts_pin': 7})
    template = {
        'schema_version': int,
        'node': {'role': str, 'id': int},
        'universal': dict.fromkeys(('strap_pin', 'parent_tx_pin', 'parent_rx_pin', 'parent_rts_pin',
                                   'worker_tx_pin', 'worker_rx_pin', 'worker_rts_pin'), int),
        'wifi': {'country': str, 'channels': list, 'dwell_ms': int},
        'ble': {'enabled': bool},
        'standalone': {'enabled': bool, 'ble_dwell_ms': int},
        'bus': {**dict.fromkeys(('enabled',), bool), **dict.fromkeys(('uart', 'tx_pin', 'rx_pin', 'rts_pin', 'baud', 'first_node_id', 'last_node_id', 'poll_timeout_ms', 'membership_lease_ms', 'assignment_lease_ms'), int)},
        'detection': {'rules_directory': str, 'enabled_categories': list},
        'following_alert': {'enabled': bool, 'distance_m': int, 'delay_ms': int, 'max_gap_ms': int, 'ignore_devices': list},
        'follower': {'enabled': bool, 'release_after_ms': int},
        'gps': {'enabled': bool, 'model': str, 'backup_battery_present': bool, **dict.fromkeys(('uart', 'rx_pin', 'tx_pin', 'baud', 'max_fix_age_ms', 'max_timing_uncertainty_ms'), int)},
        'sd': {'enabled': bool, 'format_if_mount_failed': bool, 'model': str, **dict.fromkeys(('sck_pin', 'mosi_pin', 'miso_pin', 'cs_pin', 'clock_khz'), int)},
        'logging': {'mode': str, **dict.fromkeys(('flush_interval_ms', 'rotate_bytes', 'max_files'), int)},
        'display': {'enabled': bool, **dict.fromkeys(('sda_pin', 'scl_pin', 'i2c_address', 'page_interval_ms'), int)},
        'buzzer': {'enabled': bool, **dict.fromkeys(('pin', 'frequency_hz', 'duration_ms', 'cooldown_ms'), int)},
        'signal_trend': {'enabled': bool, **dict.fromkeys(('increasing_pin', 'decreasing_pin', 'steady_pin', 'window_ms', 'stale_ms', 'minimum_samples', 'deadband_db', 'confirmations'), int)},
    }
    def structure(value, expected, prefix=''):
        if isinstance(expected, dict):
            require(type(value) is dict, f'{prefix}: expected object')
            require(value.keys() == expected.keys(), f'{prefix}: missing/unknown keys: {value.keys() ^ expected.keys()}')
            for k in expected:
                structure(value[k], expected[k], f'{prefix}.{k}')
        else:
            require(type(value) is expected, f'{prefix}: expected {expected.__name__}')
    structure(settings, template)
    require(settings['schema_version'] == 1, 'unsupported settings schema')
    if role_override is not None:
        settings['node']['role'] = role_override
    if node_override is not None:
        settings['node']['id'] = node_override
    node = settings['node']
    require(node['role'] in ('universal', 'parent', 'child', 'follower'), 'invalid role')
    universal = node['role'] == 'universal'
    require(not universal or node['id'] == 0, 'universal node ID must be 0 (assigned at runtime)')
    integer(node['id'], 0 if node['role'] in ('parent', 'universal') else 1, 16, 'node.id')
    require(node['role'] != 'parent' or node['id'] == 0, 'parent ID must be 0')
    wifi = settings['wifi']
    # Initial receive-only implementation deliberately excludes DFS / region-specific extensions.
    allowed = set(range(1, 12)) | {36, 40, 44, 48, 149, 153, 157, 161}
    require(wifi['country'] == 'US', 'initial channel policy supports US only')
    require(0 < len(wifi['channels']) <= 32, 'channels must be nonempty, unique, <=32')
    for channel in wifi['channels']:
        integer(channel, 1, 196, 'channel')
        require(channel in allowed, f'channel {channel} outside implemented passive policy')
    require(len(set(wifi['channels'])) == len(wifi['channels']), 'channels must be unique')
    integer(wifi['dwell_ms'], 10, 60000, 'wifi.dwell_ms')
    standalone = settings['standalone']
    integer(standalone['ble_dwell_ms'], 100, 60000, 'standalone.ble_dwell_ms')
    require(not standalone['enabled'] or node['role'] == 'parent',
            'standalone mode requires the parent role')
    require(not standalone['enabled'] or not settings['bus']['enabled'],
            'disable the bus for standalone mode')
    bus = settings['bus']
    for field in ('first_node_id', 'last_node_id'):
        integer(bus[field], 1, 16, f'bus.{field}')
    require(bus['first_node_id'] <= bus['last_node_id'], 'invalid bus ID range')
    require(not bus['enabled'] or node['role'] in ('parent', 'universal') or
            bus['first_node_id'] <= node['id'] <= bus['last_node_id'],
            'worker node ID must be inside the bus discovery range')
    integer(bus['poll_timeout_ms'], 20, 1000, 'bus.poll_timeout_ms')
    integer(bus['baud'], 9600, 921600, 'bus.baud')
    require(bus['poll_timeout_ms'] >= (532 * 10000 + bus['baud'] - 1) // bus['baud'] + 10, 'poll timeout too short for bus baud rate')
    integer(bus['membership_lease_ms'], 100, 60000, 'bus.membership_lease_ms')
    integer(bus['assignment_lease_ms'], 100, 60000, 'bus.assignment_lease_ms')
    sweep = (bus['last_node_id'] - bus['first_node_id'] + 1) * (bus['poll_timeout_ms'] + 5)
    if universal:
        # Each node can incur discovery query/reply and assignment/ACK in
        # addition to its poll. Include request transmission and scheduler slack.
        request_ms = (128 * 10000 + bus['baud'] - 1) // bus['baud']
        sweep = (bus['last_node_id'] - bus['first_node_id'] + 1) * (3 * (bus['poll_timeout_ms'] + request_ms) + 84)
    require(bus['membership_lease_ms'] > sweep * 2, 'membership lease must exceed two worst-case bus sweeps')
    require(bus['assignment_lease_ms'] > bus['membership_lease_ms'], 'assignment lease must exceed membership lease')
    for name in ('gps', 'bus'):
        integer(settings[name]['uart'], 0, 1, name+'.uart')
        integer(settings[name]['baud'], 9600, 921600, name+'.baud')
    require(not (settings['gps']['enabled'] and bus['enabled'] and settings['gps']['uart'] == bus['uart']), 'GPS and bus require separate UARTs')
    require(settings['gps']['model'] == 'Grove GPS Air530', 'unsupported GPS model')
    require(settings['gps']['backup_battery_present'], 'Air530 hardware profile expects backup battery')
    integer(settings['gps']['max_fix_age_ms'], 0, 60000, 'gps.max_fix_age_ms')
    integer(settings['gps']['max_timing_uncertainty_ms'], 0, 5000, 'gps.max_timing_uncertainty_ms')
    integer(settings['sd']['clock_khz'], 400, 20000, 'sd.clock_khz')
    integer(settings['display']['i2c_address'], 0x08, 0x77, 'display.i2c_address')
    integer(settings['display']['page_interval_ms'], 500, 60000, 'display.page_interval_ms')
    integer(settings['buzzer']['frequency_hz'], 100, 10000, 'buzzer.frequency_hz')
    integer(settings['buzzer']['duration_ms'], 10, 5000, 'buzzer.duration_ms')
    integer(settings['buzzer']['cooldown_ms'], 100, 3600000, 'buzzer.cooldown_ms')
    integer(settings['follower']['release_after_ms'], 1000, 3600000, 'follower.release_after_ms')
    log = settings['logging']
    require(log['mode'] in ('matches', 'all'), 'logging.mode must be matches or all')
    integer(log['flush_interval_ms'], 100, 60000, 'logging.flush_interval_ms')
    integer(log['rotate_bytes'], 4096, 1073741824, 'logging.rotate_bytes')
    integer(log['max_files'], 2, 1000, 'logging.max_files')
    trend = settings['signal_trend']
    integer(trend['window_ms'], 100, 60000, 'signal_trend.window_ms')
    integer(trend['stale_ms'], trend['window_ms'], 3600000, 'signal_trend.stale_ms')
    integer(trend['minimum_samples'], 1, 31, 'signal_trend.minimum_samples')
    integer(trend['deadband_db'], 0, 30, 'signal_trend.deadband_db')
    integer(trend['confirmations'], 1, 20, 'signal_trend.confirmations')
    following = settings['following_alert']
    integer(following['distance_m'], 1, 100000, 'following_alert.distance_m')
    integer(following['delay_ms'], 0, 3600000, 'following_alert.delay_ms')
    integer(following['max_gap_ms'], 1, 3600000, 'following_alert.max_gap_ms')
    require(not following['enabled'] or settings['gps']['enabled'], 'following alerts require GPS')
    require(len(following['ignore_devices']) <= 64, 'maximum 64 ignored devices')
    identities = set()
    for device in following['ignore_devices']:
        require(type(device) is dict and set(device) == {'radio', 'address_type', 'address'}, 'invalid ignored device keys')
        require(device['radio'] in ('wifi', 'ble'), 'invalid ignored device radio')
        integer(device['address_type'], 0, 1, 'ignored device address_type')
        require(device['radio'] != 'wifi' or device['address_type'] == 0, 'Wi-Fi address_type must be 0')
        require(type(device['address']) is str and re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', device['address']), 'ignored device needs colon-separated MAC address')
        identity = (device['radio'], device['address_type'], device['address'].lower())
        require(identity not in identities, 'duplicate ignored device')
        identities.add(identity)
    used = {}
    if universal:
        require(bus['enabled'], 'universal mode requires the bus')
        profile = settings['universal']
        require(profile['strap_pin'] == 10, 'universal strap must use verified GPIO10')
        require((profile['parent_tx_pin'], profile['parent_rx_pin'], profile['parent_rts_pin']) == (11, 3, 7),
                'universal parent bus requires verified GPIO11/3/7')
        require((profile['worker_tx_pin'], profile['worker_rx_pin'], profile['worker_rts_pin']) == (11, 12, 7),
                'universal worker bus requires verified GPIO11/12/7')
        used[profile['strap_pin']] = 'universal.strap_pin'
        # Validate parent peripherals against the runtime parent bus profile.
        used.update({profile['parent_tx_pin']: 'parent bus TX', profile['parent_rx_pin']: 'parent bus RX',
                     profile['parent_rts_pin']: 'parent bus RTS'})
    for section, values in settings.items():
        if not isinstance(values, dict) or section == 'universal' or (universal and section == 'bus'):
            continue
        for key, pin in values.items():
            if not key.endswith('_pin') and not (section == 'buzzer' and key == 'pin'):
                continue
            integer(pin, -1, 28, f'{section}.{key}')
            optional_pin = (section == 'signal_trend' or (section == 'gps' and key == 'tx_pin'))
            active = values.get('enabled', False) and not (optional_pin and pin == -1)
            if active:
                require(pin >= 0, f'{section}.{key}: assign GPIO before enabling')
                require(pin not in used, f'GPIO {pin} used by {used.get(pin)} and {section}.{key}')
                used[pin] = f'{section}.{key}'
    categories = settings['detection']['enabled_categories']
    require(all(isinstance(c, str) and re.fullmatch(r'[a-z0-9_]{1,40}', c) for c in categories), 'invalid category name')
    require(len(categories) == len(set(categories)), 'duplicate category')
    directory = (base / settings['detection']['rules_directory']).resolve()
    require(directory.is_dir(), f'rules directory missing: {directory}')
    rules, ids = [], set()
    for path in sorted(directory.glob('*.json')):
        document = load(path)
        require(type(document) is dict and set(document) == {'schema_version', 'rules'} and
                type(document['schema_version']) is int and document['schema_version'] == 1,
                f'{path}: invalid rule document')
        require(isinstance(document['rules'], list), f'{path}: rules must be a list')
        for rule in document['rules']:
            require(type(rule) is dict and set(rule) == {'id', 'category', 'enabled', 'radio', 'match', 'priority', 'confidence', 'actions', 'predicates', 'source'}, f'{path}: rule keys invalid')
            require(isinstance(rule['id'], str) and re.fullmatch(r'[a-zA-Z0-9_-]{1,64}', rule['id']) and rule['id'] not in ids, 'invalid/duplicate rule ID')
            ids.add(rule['id'])
            require(type(rule['enabled']) is bool, 'rule.enabled must be boolean')
            require(isinstance(rule['category'], str) and re.fullmatch(r'[a-z0-9_]{1,40}', rule['category']), 'invalid rule category')
            require(rule['radio'] in ('wifi', 'ble') and rule['match'] in ('all', 'any'), 'invalid radio/match')
            integer(rule['priority'], 0, 255, 'rule.priority')
            require(rule['confidence'] in ('possible', 'probable', 'broadcast'), 'invalid confidence')
            require(type(rule['actions']) is dict and set(rule['actions']) == {'buzzer', 'follow'} and all(type(v) is bool for v in rule['actions'].values()), 'invalid actions')
            require(type(rule['source']) is dict and set(rule['source']) == {'url', 'revision', 'license', 'reviewed'} and all(isinstance(v, str) and v for v in rule['source'].values()), 'source provenance required')
            require(isinstance(rule['predicates'], list) and 1 <= len(rule['predicates']) <= 4, 'rule needs 1..4 predicates')
            for pred in rule['predicates']:
                require(type(pred) is dict and set(pred) == {'field', 'value'}, 'invalid predicate')
                field, value = pred['field'], pred['value']
                require(field in ('oui', 'ssid_prefix', 'name_prefix', 'manufacturer_prefix', 'service_uuid16', 'service_uuid128',
                                  'rid_valid', 'rid_message_type', 'rid_basic_id', 'rid_basic_id_type',
                                  'rid_position_valid'), 'unknown predicate field')
                require(isinstance(value, str) and 0 < len(value) <= 96, 'invalid predicate value')
                if field in ('rid_valid', 'rid_position_valid'):
                    require(value == 'true', f'{field} must be "true"')
                if field == 'rid_message_type':
                    require(value in ('basic_id', 'location', 'authentication', 'self_id', 'system', 'operator_id'),
                            'invalid Remote ID message type')
                if field == 'rid_basic_id_type':
                    require(value in ('1', '2', '3', '4'), 'Remote ID Basic ID type must be 1..4')
                if field == 'rid_basic_id':
                    require(1 <= len(value) <= 20 and all(32 <= ord(c) <= 126 for c in value),
                            'Remote ID Basic ID must be 1..20 printable ASCII characters')
                if field in ('ssid_prefix', 'name_prefix'):
                    require(len(value.encode('utf-8')) <= 32 and
                            all(ord(c) >= 32 and ord(c) != 127 for c in value),
                            'text predicate must be printable UTF-8 within the 32-byte captured text limit')
                require(not (rule['radio'] == 'wifi' and field in ('name_prefix', 'manufacturer_prefix', 'service_uuid16', 'service_uuid128')), 'BLE predicate in Wi-Fi rule')
                require(not (rule['radio'] == 'ble' and field == 'ssid_prefix'), 'SSID predicate in BLE rule')
                if field == 'service_uuid128':
                    require(bool(re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', value)),
                            'UUID128 must be canonical 8-4-4-4-12 hex text')
                    pred['value'] = value.lower()
                if field in ('oui', 'manufacturer_prefix', 'service_uuid16'):
                    require(bool(re.fullmatch(r'[0-9a-fA-F :\-]+', value)), 'invalid hex predicate')
                    raw = bytes.fromhex(value.replace(':', '').replace('-', ''))
                    require(0 < len(raw) <= 32, 'hex predicate size')
                    require(field != 'oui' or len(raw) == 3, 'OUI must be 3 bytes')
                    require(field != 'service_uuid16' or len(raw) == 2, 'UUID16 must be 2 bytes, big-endian text')
            rule['enabled'] = rule['enabled'] and rule['category'] in categories
            rules.append(rule)
    require(len(rules) <= 64, 'maximum 64 rules')
    return settings, rules


def emit(settings, rules):
    def cpp(value):
        if isinstance(value, bool):
            return str(value).lower()
        if isinstance(value, str):
            # C++ universal character escapes cannot encode NUL or UTF-16 surrogate
            # pairs. Fixed-width octal escapes preserve arbitrary UTF-8 safely;
            # escaping '?' also prevents trigraph warnings under -Werror.
            return '"' + ''.join(chr(b) if 32 <= b < 127 and b not in (34, 63, 92)
                                 else f'\\{b:03o}' for b in value.encode('utf-8')) + '"'
        return str(value)
    out = ['// Generated from config/settings.json and rules/*.json. Do not edit.', '#pragma once', '#include "wiknow/core.hpp"', 'namespace wiknow::config {']
    for section, fields in settings.items():
        if not isinstance(fields, dict):
            continue
        for key, value in fields.items():
            if isinstance(value, (bool, int, str)):
                kind = 'bool' if isinstance(value, bool) else 'int' if isinstance(value, int) else 'auto'
                out.append(f'inline constexpr {kind} {section}_{key} = {cpp(value)};')
    ignored = settings['following_alert']['ignore_devices']
    out.append(f'inline constexpr std::array<DeviceIdentity, {len(ignored)}> following_ignore = {{{{')
    for device in ignored:
        address = ','.join('0x'+part for part in device['address'].split(':'))
        out.append('DeviceIdentity{Radio::'+device['radio']+','+str(device['address_type'])+',{{'+address+'}}},')
    out.append('}};')
    out.append('inline constexpr FollowingConfig following_config{following_alert_enabled, following_alert_distance_m, following_alert_delay_ms, following_alert_max_gap_ms};')
    universal = settings['node']['role'] == 'universal'
    out.append(f'inline constexpr bool universal_enabled = {cpp(universal)};')
    role = 'child' if universal else settings['node']['role']
    out.append(f'inline constexpr Role role = Role::{role};')
    channels = ','.join('Channel{'+str(n)+'}' for n in settings['wifi']['channels'])
    out.append(f'inline constexpr std::array<Channel, {len(settings["wifi"]["channels"])}> channels = {{{channels}}};')
    out.append('inline constexpr TrendConfig trend_config{signal_trend_window_ms, signal_trend_stale_ms, signal_trend_minimum_samples, signal_trend_confirmations, signal_trend_deadband_db};')
    out.append(f'inline constexpr std::array<Rule, {len(rules)}> rules = {{{{')
    for rule in rules:
        preds = ','.join('Predicate{Field::'+p['field']+','+cpp(p['value'])+'}' for p in rule['predicates'])
        out.append('Rule{'+','.join([cpp(rule['id']), cpp(rule['category']), 'Radio::'+rule['radio'], cpp(rule['enabled']), cpp(rule['match']=='all'), str(rule['priority']), cpp(rule['actions']['buzzer']), cpp(rule['actions']['follow']), '{{'+preds+'}}', str(len(rule['predicates'])), cpp(rule['confidence'])])+'},')
    out.extend(['}};', '}'])
    return '\n'.join(out)+'\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=pathlib.Path, default=ROOT/'config/settings.json')
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--role', choices=['universal', 'parent', 'child', 'follower'])
    parser.add_argument('--node-id', type=int)
    args = parser.parse_args()
    try:
        settings, rules = validate(load(args.config), args.role, args.node_id)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            content = emit(settings, rules)
            if not args.output.exists() or args.output.read_text(encoding='utf-8') != content:
                args.output.write_text(content, encoding='utf-8')
            # Keep the reviewed source and predicates alongside firmware artifacts
            # without repeating potentially long metadata in each radio event.
            manifest = args.output.with_suffix('.rules.json')
            content = json.dumps({'schema_version': 1, 'rules': rules},
                                 ensure_ascii=True, indent=2) + '\n'
            if not manifest.exists() or manifest.read_text(encoding='utf-8') != content:
                manifest.write_text(content, encoding='utf-8')
        print(f'Validated {settings["node"]["role"]} node {settings["node"]["id"]}: {len(rules)} rules ({sum(r["enabled"] for r in rules)} enabled)')
        if not any(r['enabled'] for r in rules):
            print('NOTE: no enabled detection rules; match alerts unavailable. See rules/README.md.')
        available = {r['category'] for r in rules if r['enabled']}
        unavailable = sorted(set(settings['detection']['enabled_categories']) - available)
        if unavailable:
            print('NOTE: no enabled rules for categories: ' + ', '.join(unavailable))
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(f'Configuration error: {error}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
