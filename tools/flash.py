#!/usr/bin/env python3
"""Build the universal wi-know ESP32-C5 image and flash one USB-connected board."""
import argparse
import glob
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

# Supports both `python tools/flash.py` and import by host tests.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_config

ROOT = Path(__file__).resolve().parents[1]
SUPPORTED_IDF = '5.5.1'
ROLES = ('universal', 'parent', 'child', 'follower')


def parser():
    result = argparse.ArgumentParser(description=__doc__, epilog=(
        'The default universal image detects its identity at boot. Activate ESP-IDF v5.5.1 first. '
        'No full-chip erase is performed. See docs/flashing.md.'))
    result.add_argument('--role', choices=ROLES)
    result.add_argument('--node-id', type=int, help='Parent: 0; children/follower: unique bus ID')
    result.add_argument('--port', help='Serial device, e.g. /dev/ttyACM0 or COM5')
    result.add_argument('--config', type=Path, default=ROOT/'config/settings.json')
    result.add_argument('--idf-path', type=Path, help='ESP-IDF checkout; its environment must be activated')
    result.add_argument('--build-only', action='store_true', help='Build without opening a serial port')
    result.add_argument('--monitor', action='store_true', help='Open IDF serial monitor after flashing (Ctrl+] exits)')
    result.add_argument('--baud', type=int, default=460800, help='Flash baud rate; use 115200 if unreliable')
    result.add_argument('--dry-run', action='store_true', help='Validate and print commands; never build or touch hardware')
    result.add_argument('--list-ports', action='store_true', help='List detected serial ports and exit')
    return result


def serial_ports():
    """Optional pyserial (included in IDF); dependency-free platform fallback."""
    try:
        from serial.tools import list_ports
        return sorted((p.device, p.description or '') for p in list_ports.comports())
    except ImportError:
        pass
    if os.name == 'nt':
        import winreg
        found = []
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DEVICEMAP\SERIALCOMM') as key:
                index = 0
                while True:
                    try:
                        _, port, _ = winreg.EnumValue(key, index)
                        found.append((port, 'serial device'))
                        index += 1
                    except OSError:
                        break
        except FileNotFoundError:
            pass
        return sorted(found)
    paths = set()
    for pattern in ('/dev/ttyACM*', '/dev/ttyUSB*', '/dev/cu.*', '/dev/serial/by-id/*'):
        paths.update(glob.glob(pattern))
    # Linux by-id symlinks often identify the same tty already found above.
    unique = {}
    for path in sorted(paths):
        unique.setdefault(os.path.realpath(path), path)
    return [(path, 'serial device') for path in unique.values()]


def show_ports(ports):
    if not ports:
        print('No serial ports detected. Connect a board with a USB data cable.')
    for index, (port, description) in enumerate(ports, 1):
        print(f'  {index}. {port}  {description}')


def prompt(text, default=None):
    if not sys.stdin.isatty():
        raise ValueError('Interactive input unavailable. Supply --role, --node-id (for workers), and --port; '
                         'or use --build-only/--dry-run.')
    suffix = f' [{default}]' if default is not None else ''
    answer = input(f'{text}{suffix}: ').strip()
    if not answer and default is not None:
        return str(default)
    return answer


def select_id(role, first, last):
    if role in ('parent', 'universal'):
        return 0
    print(f'Choose a unique node ID in {first}–{last}; children and followers share the same ID space.')
    default = first if role == 'child' else min(first+1, last)
    while True:
        answer = prompt('Node ID', default)
        try:
            value = int(answer)
            if first <= value <= last:
                return value
        except ValueError:
            pass
        print(f'Enter a whole number from {first} to {last}.')


def select_port():
    ports = serial_ports()
    print('\nUSB / serial ports:')
    show_ports(ports)
    while True:
        answer = prompt('Select port number, enter a device path, or r to rescan', '1' if ports else None)
        if answer.lower() == 'r':
            ports = serial_ports()
            show_ports(ports)
        elif answer.isdigit():
            if 1 <= int(answer) <= len(ports):
                return ports[int(answer)-1][0]
            print('Choose a listed number, or enter the serial device path.')
        elif answer:
            return answer


def idf_command(idf_path=None, dry_run=False):
    """Use the activated IDF environment; do not source shell code or install packages."""
    explicit = idf_path or os.environ.get('IDF_PATH')
    if explicit:
        script = Path(explicit).expanduser().resolve()/'tools/idf.py'
        if not script.is_file():
            raise ValueError(f'ESP-IDF script not found: {script}')
    else:
        discovered = shutil.which('idf.py')
        if not discovered:
            if dry_run:
                return [sys.executable, 'idf.py']
            raise ValueError('ESP-IDF is not active. Install v5.5.1 and run its export.sh '
                             '(or open an ESP-IDF terminal on Windows), then retry. See docs/flashing.md.')
        script = Path(discovered)
    executable = sys.executable
    env_path = os.environ.get('IDF_PYTHON_ENV_PATH')
    if env_path:
        candidate = Path(env_path)/('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
        if candidate.is_file():
            executable = str(candidate)
    return [executable, str(script)]


def build_directory(role, node_id, config):
    # Different config paths get different caches; content changes rebuild incrementally.
    digest = hashlib.sha256(str(config.resolve()).encode()).hexdigest()[:10]
    return ROOT/'build'/'devices'/f'{role}-{node_id}-{digest}'


def commands(idf, role, node_id, config, port, baud, build_only, monitor):
    directory = build_directory(role, node_id, config)
    common = [*idf, '-C', str(ROOT/'firmware'), '-B', str(directory),
              '-DIDF_TARGET=esp32c5', f'-DSDKCONFIG={directory / "sdkconfig"}',
              f'-DWIKNOW_ROLE={role}', f'-DWIKNOW_NODE_ID={node_id}',
              f'-DWIKNOW_CONFIG={config.resolve()}']
    result = [[*common, 'build']]
    if not build_only:
        result.append([*common, '-p', port, '-b', str(baud), 'flash'])
        if monitor:
            result.append([*common, '-p', port, 'monitor'])
    return result


def display_command(command):
    return subprocess.list2cmdline(command) if os.name == 'nt' else shlex.join(command)


def check_idf(idf):
    result = subprocess.run([*idf, '--version'], cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise ValueError('ESP-IDF could not start. Activate its installed Python/toolchain environment.\n' +
                         (result.stderr or result.stdout).strip())
    version = re.search(r'ESP-IDF\s+v(\d+\.\d+\.\d+)(?=\s|$)', result.stdout)
    if not version or version.group(1) != SUPPORTED_IDF:
        raise ValueError(f'Expected ESP-IDF v{SUPPORTED_IDF}; got {result.stdout.strip()!r}. '
                         'Activate the pinned SDK before flashing.')


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.list_ports:
            show_ports(serial_ports())
            return 0
        if args.build_only and (args.port or args.monitor):
            raise ValueError('--build-only cannot be combined with --port or --monitor.')
        if args.baud < 9600 or args.baud > 2000000:
            raise ValueError('--baud must be between 9600 and 2000000.')
        config = args.config.expanduser().resolve()
        raw = generate_config.load(config)
        # The menu/CLI selects role and ID independently of the stored defaults.
        # Validate the common settings first; validate the selected device below.
        generate_config.validate(raw, 'parent', 0)
        role = args.role or 'universal'
        first, last = raw['bus']['first_node_id'], raw['bus']['last_node_id']
        node_id = args.node_id
        if node_id is None:
            node_id = select_id(role, first, last)
        _, rules = generate_config.validate(raw, role, node_id)
        if role not in ('parent', 'universal') and not first <= node_id <= last:
            raise ValueError(f'Node ID {node_id} is outside the configured bus discovery range {first}–{last}.')
        port = args.port
        if port is not None and (not port.strip() or port.startswith('-')):
            raise ValueError('Invalid serial port.')
        if not args.build_only and port is None:
            port = '<serial-port>' if args.dry_run else select_port()
        idf = idf_command(args.idf_path, args.dry_run)
        planned = commands(idf, role, node_id, config, port, args.baud, args.build_only, args.monitor)
        print(f'\nDevice: {role} / node {node_id} / ESP32-C5')
        print(f'Config: {config}')
        print(f'Build:  {build_directory(role, node_id, config)}')
        if port:
            print(f'Port:   {port}')
        if not any(rule['enabled'] for rule in rules):
            print('NOTE: no enabled detection rules. Add signatures under rules/ to enable match alerts.')
        if args.dry_run:
            print('\nDRY RUN — commands only; no build, flash, or device access:')
            for command in planned:
                print(display_command(command))
            return 0
        check_idf(idf)
        print('\nBuilding firmware; flashing starts only after a successful build.' if not args.build_only
              else '\nBuilding firmware without flashing.')
        for command in planned:
            print('\n> ' + display_command(command), flush=True)
            result = subprocess.run(command, cwd=ROOT)
            if result.returncode:
                print(f'Command failed (exit {result.returncode}); remaining steps skipped.', file=sys.stderr)
                if 'flash' in command:
                    print('Check USB data cable/port permissions. If needed, hold BOOT, tap RESET, '
                          'release BOOT, then retry; --baud 115200 may help.', file=sys.stderr)
                return result.returncode if result.returncode > 0 else 1
            if command[-1] == 'flash':
                print(f'\nFlashed {role} node {node_id} successfully.', flush=True)
        if args.build_only:
            print('\nBuild complete.')
        return 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print('\nCancelled.', file=sys.stderr)
        return 130


if __name__ == '__main__':
    sys.exit(main())
