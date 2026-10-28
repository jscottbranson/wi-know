#!/usr/bin/env python3
"""Back up internal flash logs over USB and export detections in one command."""
import argparse
import csv
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_observations
import flash


def log_partition():
    """Use the repository's explicit partition layout, shared with firmware."""
    with (flash.ROOT / 'firmware/partitions.csv').open() as source:
        for row in csv.reader(line for line in source if not line.lstrip().startswith('#')):
            if row and row[0].strip() == 'logs':
                return int(row[3].strip(), 0), int(row[4].strip(), 0)
    raise ValueError('No logs partition in firmware/partitions.csv')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--port', help='USB serial port; prompts when omitted')
    source.add_argument('--image', type=Path, help='export an existing logs.bin without accessing hardware')
    parser.add_argument('--directory', type=Path, help='new backup directory; default: tmp/flash-export-* in the repository')
    parser.add_argument('--format', choices=('json', 'jsonl', 'csv', 'tsv', 'geojson'), default='json')
    parser.add_argument('--location', choices=('capture', 'peak', 'aircraft', 'operator'), default='capture')
    parser.add_argument('--matches-only', action='store_true')
    parser.add_argument('--skip-invalid', action='store_true', help='skip malformed records with warnings')
    parser.add_argument('--idf-path', type=Path, help='activated ESP-IDF v5.5.1 checkout')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--manual-reset', action='store_true', help='disable automatic reset for boards requiring BOOT/RESET')
    args = parser.parse_args(argv)
    directory = None
    try:
        if args.baud <= 0:
            raise ValueError('--baud must be positive')
        offset, size = log_partition()
        if args.image and (not args.image.is_file() or args.image.stat().st_size != size):
            raise ValueError(f'--image must be a {size}-byte logs partition backup')
        idf = flash.idf_command(args.idf_path)
        flash.check_idf(idf)
        extractor = Path(idf[1]).resolve().parent.parent / 'components/fatfs/fatfsparse.py'
        if not extractor.is_file():
            raise ValueError(f'ESP-IDF FAT parser not found: {extractor}')
        port = None if args.image else (args.port or flash.select_port())
        if args.directory:
            directory = args.directory.resolve()
            directory.mkdir(parents=True, exist_ok=False)
        else:
            backup_root = flash.ROOT / 'tmp'
            backup_root.mkdir(exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix='flash-export-', dir=backup_root))
        print(f'Backup directory: {directory}', flush=True)
        backup = directory / 'logs.bin'
        if args.image:
            shutil.copyfile(args.image, backup)
        else:
            print('Close serial monitors before export. Reset can lose buffered records.', flush=True)
            command = [idf[0], '-m', 'esptool', '--chip', 'esp32c5', '--port', port,
                       '--baud', str(args.baud), '--before',
                       'no_reset' if args.manual_reset else 'default_reset',
                       '--after', 'no_reset' if args.manual_reset else 'hard_reset',
                       '--no-stub', 'read_flash', hex(offset), hex(size), str(backup)]
            subprocess.run(command, check=True)
        if backup.stat().st_size != size:
            raise ValueError(f'Incomplete backup: expected {size} bytes; retained for inspection')
        extracted = directory / 'extracted'
        extracted.mkdir()
        subprocess.run([idf[0], str(extractor), '--wl-layer', 'enabled', str(backup)],
                       cwd=extracted, check=True)
        logs = sorted((path for path in extracted.rglob('*')
                       if path.is_file() and re.fullmatch(r'WK[0-9]{8}\.LOG', path.name, re.IGNORECASE)),
                      key=lambda path: str(path).upper())
        if not logs:
            raise ValueError('No WK########.LOG files found; raw backup retained')
        destination = directory / f'detections.{args.format}'
        options = [str(path) for path in logs] + ['-o', str(destination), '--location', args.location]
        for flag in ('matches_only', 'skip_invalid'):
            if getattr(args, flag):
                options.append('--' + flag.replace('_', '-'))
        if export_observations.main(options):
            raise ValueError('Detection export failed; raw backup and extracted logs retained')
        print(f'Exported {len(logs)} log file(s): {destination}')
        if args.manual_reset and not args.image:
            print('Tap RESET with BOOT released to resume scanning.')
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Flash export failed: {error}', file=sys.stderr)
        if directory:
            print(f'Backup files retained in {directory}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('Flash export interrupted; any backup files are retained.', file=sys.stderr)
        return 130


if __name__ == '__main__':
    sys.exit(main())
