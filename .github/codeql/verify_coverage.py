"""Fail CI when CodeQL silently misses our firmware, GATT path, or host build."""
import csv
from pathlib import Path
import sys


def main():
    with Path(sys.argv[1]).open(newline='') as stream:
        rows = {(row['file'], row['function_name']) for row in csv.DictReader(stream)}
    expected = {
        'firmware/main/main.cpp',
        'firmware/main/radios.cpp',
        'firmware/main/peripherals.cpp',
        'components/wiknow/src/core.cpp',
        'components/wiknow/src/remote_id.cpp',
        'tests/core_tests.cpp',
    }
    missing = expected - {path for path, _ in rows}
    if missing:
        raise SystemExit('CodeQL did not extract function bodies from: ' + ', '.join(sorted(missing)))
    if ('firmware/main/radios.cpp', 'gatt_service') not in rows:
        raise SystemExit('CodeQL did not extract the optional GATT implementation')
    print('CodeQL extracted host/core, all firmware sources, and the optional GATT implementation.')


if __name__ == '__main__':
    main()
