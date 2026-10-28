"""Exercise the USB/backup pipeline without attached hardware or an SDK."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import export_flash


class FlashExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.idf = self.root / 'idf'
        parser = self.idf / 'components/fatfs/fatfsparse.py'
        parser.parent.mkdir(parents=True)
        parser.touch()
        self.output = self.root / 'backup'
        self.size = export_flash.log_partition()[1]
        self.calls = []
        self.malformed = False
        self.no_logs = False
        self.fail_read = False
        self.short_read = False
        for name, kwargs in (
            ('idf_command', {'return_value': [sys.executable, str(self.idf / 'tools/idf.py')]}),
            ('check_idf', {}),
            ('select_port', {'return_value': '/dev/test'}),
        ):
            mock = patch.object(export_flash.flash, name, **kwargs)
            mock.start()
            self.addCleanup(mock.stop)
        mock = patch.object(export_flash.subprocess, 'run', side_effect=self.run_command)
        mock.start()
        self.addCleanup(mock.stop)

    def run_command(self, command, **kwargs):
        self.calls.append(command)
        if 'esptool' in command:
            Path(command[-1]).write_bytes(b'x' * (10 if self.short_read else self.size))
            if self.fail_read:
                raise subprocess.CalledProcessError(2, command)
        elif not self.no_logs:
            volume = Path(kwargs['cwd']) / 'LOGS'
            volume.mkdir()
            record = {'schema_version': 1, 'radio': 'wifi', 'address': 'test', 'sequence': 2}
            # IDF's FAT tooling can produce lowercase long filenames.
            (volume / 'wk00000002.log').write_text(json.dumps(record) + '\n')
            record['sequence'] = 1
            (volume / 'WK00000001.LOG').write_text(json.dumps(record) + '\n' + ('{"broken"' if self.malformed else ''))
            (volume / 'unrelated.LOG').write_text('invalid')
        return subprocess.CompletedProcess(command, 0)

    def invoke(self, *args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return export_flash.main(['--directory', str(self.output), *args])

    def test_usb_read_automatically_resets_and_exports_in_order(self):
        self.assertEqual(self.invoke('--port', '/dev/test'), 0)
        command = self.calls[0]
        self.assertIn('default_reset', command)
        self.assertIn('hard_reset', command)
        self.assertEqual(command[-4:-1], ['read_flash', '0x180000', '0x80000'])
        self.assertEqual((self.output / 'logs.bin').stat().st_size, self.size)
        rows = json.loads((self.output / 'detections.json').read_text())
        self.assertEqual([r['sequence'] for r in rows], [1, 2])

    def test_default_exports_use_unique_directories_under_repository_tmp(self):
        partition = export_flash.log_partition()
        with patch.object(export_flash.flash, 'ROOT', self.root), \
                patch.object(export_flash, 'log_partition', return_value=partition), \
                patch.object(Path, 'cwd', side_effect=AssertionError('must not depend on cwd')), \
                contextlib.redirect_stdout(io.StringIO()):
            for _ in range(2):
                self.assertEqual(export_flash.main(['--port', '/dev/test']), 0)
        backups = list((self.root / 'tmp').glob('flash-export-*'))
        self.assertEqual(len(backups), 2)
        for backup in backups:
            self.assertTrue((backup / 'detections.json').is_file())
            self.assertEqual((backup / 'logs.bin').stat().st_size, self.size)

    def test_offline_image_preserved_and_csv_exported(self):
        image = self.root / 'original.bin'
        image.write_bytes(b'z' * self.size)
        self.assertEqual(self.invoke('--image', str(image), '--format', 'csv'), 0)
        self.assertEqual(len(self.calls), 1)
        self.assertEqual((self.output / 'logs.bin').read_bytes(), image.read_bytes())
        self.assertTrue((self.output / 'detections.csv').read_text().startswith('source_file,'))

    def test_existing_directory_is_never_overwritten(self):
        self.output.mkdir()
        sentinel = self.output / 'logs.bin'
        sentinel.write_bytes(b'keep')
        self.assertEqual(self.invoke('--port', '/dev/test'), 1)
        self.assertFalse(self.calls)
        self.assertEqual(sentinel.read_bytes(), b'keep')

    def test_failed_read_preserves_partial_backup_without_extracting(self):
        self.fail_read = True
        self.assertEqual(self.invoke('--port', '/dev/test'), 1)
        self.assertEqual(len(self.calls), 1)
        self.assertTrue((self.output / 'logs.bin').is_file())

    def test_short_read_is_not_extracted(self):
        self.short_read = True
        self.assertEqual(self.invoke('--port', '/dev/test'), 1)
        self.assertEqual(len(self.calls), 1)

    def test_no_logs_is_an_error_with_backup_retained(self):
        self.no_logs = True
        self.assertEqual(self.invoke('--port', '/dev/test'), 1)
        self.assertTrue((self.output / 'logs.bin').is_file())
        self.assertFalse((self.output / 'detections.json').exists())

    def test_invalid_records_preserve_logs_and_require_explicit_skip(self):
        self.malformed = True
        self.assertEqual(self.invoke('--port', '/dev/test'), 1)
        self.assertTrue((self.output / 'extracted/LOGS/WK00000001.LOG').exists())
        self.assertFalse((self.output / 'detections.json').exists())
        self.output = self.root / 'retry'
        self.assertEqual(self.invoke('--port', '/dev/test', '--skip-invalid'), 0)

    def test_manual_reset_is_explicit(self):
        self.assertEqual(self.invoke('--port', '/dev/test', '--manual-reset'), 0)
        self.assertEqual(self.calls[0].count('no_reset'), 2)

    def test_wrong_size_image_fails_before_hardware_access(self):
        image = self.root / 'short.bin'
        image.write_bytes(b'bad')
        self.assertEqual(self.invoke('--image', str(image)), 1)
        self.assertFalse(self.calls)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
