import contextlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('wiknow_flash', ROOT/'tools/flash.py')
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class FlashTests(unittest.TestCase):
    def test_supported_installations_infer_roles_and_capture_pins(self):
        for variant, board, role, switch in [
                ('standalone', 'n32r8', 'parent', 'GPIO26'),
                ('standalone', 'xiao', 'parent', 'GPIO4'),
                ('parent', 'n32r8', 'parent', 'GPIO26'),
                ('child', 'xiao', 'child', 'disabled')]:
            with self.subTest(variant=variant, board=board):
                status, output = self.run_main(
                    '--config', str(ROOT/f'config/settings.{variant}.example.json'),
                    '--board', board, '--dry-run')
                self.assertEqual(status, 0, output)
                self.assertIn(f'Device: {role}', output)
                self.assertIn(f'Capture switch: {switch}', output)
                self.assertIn(f'-DWIKNOW_BOARD={board}', output)
                self.assertIn(f'-DWIKNOW_ROLE={role}', output)

    def test_child_image_needs_no_id_or_prompt(self):
        builds = []
        for port in ('/dev/ttyACM1', '/dev/ttyACM2'):
            with patch.object(flash, 'prompt', side_effect=AssertionError('Unexpected prompt')), \
                 patch.object(flash, 'idf_command', return_value=['python', 'idf.py']), \
                 patch.object(flash, 'check_idf'), \
                 patch.object(flash.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as execute:
                status, output = self.run_main(
                    '--port', port, '--config', str(ROOT/'config/settings.child.example.json'),
                    '--board', 'xiao')
                self.assertEqual(status, 0, output)
                self.assertIn('assigned by parent at runtime', output)
                builds.append(execute.call_args_list[0].args[0])
                self.assertEqual([call.args[0][-1] for call in execute.call_args_list], ['build', 'flash'])
        self.assertEqual(builds[0], builds[1])

    def test_board_selection_remaps_standalone_and_reaches_every_stage(self):
        config = ROOT/'config/settings.standalone.example.json'
        status, output = self.run_main('--config', str(config), '--board', 'n32r8', '--dry-run')
        self.assertEqual(status, 0, output)
        self.assertIn('Device: parent', output)
        self.assertIn('sda_pin=4, scl_pin=5', output)
        self.assertIn('buzzer: pin=9', output)
        for board in ('custom', 'xiao', 'n32r8'):
            commands = flash.commands(['idf.py'], 'parent', config, 'COM7', 115200,
                                      False, True, board)
            for command in commands:
                self.assertIn(f'-DWIKNOW_BOARD={board}', command)
        self.assertEqual(len({flash.build_directory('parent', config, board)
                              for board in ('custom', 'xiao', 'n32r8')}), 3)
        status, output = self.run_main('--board', 'n32r8', '--role', 'universal', '--dry-run')
        self.assertEqual(status, 1)
        self.assertIn('manual role', output)

    def test_board_from_config_reaches_build(self):
        with tempfile.TemporaryDirectory() as folder:
            config = pathlib.Path(folder)/'settings.json'
            settings = flash.generate_config.load(ROOT/'config/settings.standalone.example.json')
            settings['board'] = {'model': 'n32r8'}
            config.write_text(json.dumps(settings))
            status, output = self.run_main('--config', str(config), '--dry-run')
            self.assertEqual(status, 0, output)
            self.assertIn('-DWIKNOW_BOARD=n32r8', output)

    def test_changed_sdk_defaults_select_fresh_sdkconfig(self):
        with tempfile.TemporaryDirectory() as folder:
            defaults = pathlib.Path(folder)/'sdkconfig.defaults'
            config = pathlib.Path(folder)/'settings.json'
            defaults.write_text('CONFIG_SPIRAM=n\n')
            config.write_text('{}')
            with patch.object(flash, 'SDK_DEFAULTS', defaults):
                old = flash.build_directory('parent', config)
                config.write_text('{"edited": true}')
                self.assertEqual(old, flash.build_directory('parent', config))
                defaults.write_text('CONFIG_SPIRAM=y\n')
                new = flash.build_directory('parent', config)
                self.assertNotEqual(old, new)
                self.assertEqual(new, flash.build_directory('parent', config))

    def test_changed_sdk_release_selects_fresh_build(self):
        config = ROOT/'config/settings.child.example.json'
        current = flash.build_directory('child', config, 'xiao')
        with patch.object(flash, 'SUPPORTED_IDF', '5.5.5'):
            previous = flash.build_directory('child', config, 'xiao')
        self.assertNotEqual(previous, current)
        self.assertEqual(current, flash.build_directory('child', config, 'xiao'))

    def test_sd_formatting_warning_in_dry_run(self):
        with tempfile.TemporaryDirectory() as folder:
            config = pathlib.Path(folder)/'settings.json'
            settings = flash.generate_config.load(ROOT/'config/settings.standalone.example.json')
            settings['sd']['format_if_mount_failed'] = True
            config.write_text(json.dumps(settings))
            status, output = self.run_main('--config', str(config), '--role', 'parent', '--dry-run')
            self.assertEqual(status, 0, output)
            self.assertIn('SD formatting is enabled', output)
            settings['sd']['format_if_mount_failed'] = False
            config.write_text(json.dumps(settings))
            status, output = self.run_main('--config', str(config), '--role', 'parent', '--dry-run')
            self.assertEqual(status, 0, output)
            self.assertNotIn('SD formatting is enabled', output)

    def run_main(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            status = flash.main(list(args))
        return status, output.getvalue()

    def test_role_and_config_are_pinned_in_every_stage(self):
        config = ROOT/'config/settings.json'
        commands = flash.commands(['python', 'idf.py'], 'child', config, 'COM7', 115200, False, True)
        self.assertEqual([command[-1] for command in commands], ['build', 'flash', 'monitor'])
        for command in commands:
            self.assertIn('-DWIKNOW_ROLE=child', command)

            self.assertIn(f'-DWIKNOW_CONFIG={config.resolve()}', command)
            self.assertIn('-DIDF_TARGET=esp32c5', command)
            sdk = next(arg for arg in command if arg.startswith('-DSDKCONFIG='))
            self.assertTrue(sdk.endswith('/sdkconfig'))
        self.assertIn('COM7', commands[1])
        self.assertIn('115200', commands[1])
        self.assertNotIn('erase-flash', commands[1])

    def test_build_directories_isolate_roles_and_configs(self):
        config = ROOT/'config/settings.json'
        directories = {
            flash.build_directory('parent', config),
            flash.build_directory('child', config),
            flash.build_directory('follower', config),
            flash.build_directory('child', ROOT/'config/settings.child.example.json'),
        }
        self.assertEqual(len(directories), 4)
        self.assertTrue(all(ROOT/'build' in path.parents for path in directories))

    def test_dry_run_never_calls_subprocess_or_serial_discovery(self):
        with patch.object(flash, 'idf_command', return_value=['python', 'idf.py']), \
             patch.object(flash.subprocess, 'run') as execute, \
             patch.object(flash, 'serial_ports') as ports:
            status, output = self.run_main('--role', 'parent', '--dry-run')
        self.assertEqual(status, 0)
        self.assertIn('DRY RUN', output)
        self.assertIn('<serial-port>', output)
        execute.assert_not_called()
        ports.assert_not_called()

    def test_build_failure_never_flashes(self):
        with patch.object(flash, 'idf_command', return_value=['python', 'idf.py']), \
             patch.object(flash, 'check_idf'), \
             patch.object(flash.subprocess, 'run', return_value=subprocess.CompletedProcess([], 2)) as execute:
            status, output = self.run_main('--role', 'child', '--port', 'COM7', '--monitor')
        self.assertEqual(status, 2)
        self.assertEqual(execute.call_count, 1)
        self.assertEqual(execute.call_args.args[0][-1], 'build')
        self.assertIn('remaining steps skipped', output)

    def test_flash_failure_never_opens_monitor(self):
        outcomes = [subprocess.CompletedProcess([], 0), subprocess.CompletedProcess([], 3)]
        with patch.object(flash, 'idf_command', return_value=['python', 'idf.py']), \
             patch.object(flash, 'check_idf'), \
             patch.object(flash.subprocess, 'run', side_effect=outcomes) as execute:
            status, _ = self.run_main('--role', 'follower', '--port', 'COM7', '--monitor')
        self.assertEqual(status, 3)
        self.assertEqual([call.args[0][-1] for call in execute.call_args_list], ['build', 'flash'])

    def test_successful_build_flash_monitor_order(self):
        with patch.object(flash, 'idf_command', return_value=['python', 'idf.py']), \
             patch.object(flash, 'check_idf'), \
             patch.object(flash.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as execute:
            status, _ = self.run_main('--role', 'parent', '--port', 'COM7', '--monitor')
        self.assertEqual(status, 0)
        self.assertEqual([call.args[0][-1] for call in execute.call_args_list], ['build', 'flash', 'monitor'])

    def test_build_only_never_selects_port(self):
        with patch.object(flash, 'idf_command', return_value=['python', 'idf.py']), \
             patch.object(flash, 'serial_ports') as ports:
            status, output = self.run_main('--role', 'child', '--build-only', '--dry-run')
        self.assertEqual(status, 0)
        self.assertNotIn('<serial-port>', output)
        ports.assert_not_called()

    def test_selected_role_overrides_stored_role(self):
        with tempfile.TemporaryDirectory() as folder:
            config = pathlib.Path(folder)/'settings.json'
            settings = flash.generate_config.load(ROOT/'config/settings.json')
            settings['node'] = {'role': 'parent'}
            config.write_text(json.dumps(settings))
            with patch.object(flash, 'idf_command', return_value=['python', 'idf.py']):
                status, output = self.run_main('--config', str(config), '--role', 'child',
                                               '--build-only', '--dry-run')
            self.assertEqual(status, 0, output)
            self.assertIn('Device: child / ESP32-C5', output)

    def test_invalid_option_combinations(self):
        cases = [('--build-only', '--monitor'),
                 ('--build-only', '--port', 'COM7')]
        with patch.object(flash.subprocess, 'run') as execute:
            for args in cases:
                status, output = self.run_main(*args, '--dry-run')
                self.assertEqual(status, 1, output)
        execute.assert_not_called()

    def test_noninteractive_defaults_to_same_universal_image(self):
        with patch.object(sys.stdin, 'isatty', return_value=False), \
             patch.object(flash, 'idf_command', return_value=['python', 'idf.py']):
            status, output = self.run_main('--dry-run')
        self.assertEqual(status, 0, output)
        self.assertIn('Device: universal / ESP32-C5', output)
        self.assertIn('-DWIKNOW_ROLE=universal', output)

        first = flash.commands(['idf.py'], 'universal', ROOT/'config/settings.json', 'COM7', 460800, False, False)
        second = flash.commands(['idf.py'], 'universal', ROOT/'config/settings.json', 'COM8', 460800, False, False)
        self.assertEqual(first[0], second[0])
        self.assertEqual(first[1][:-5], second[1][:-5])

    def test_standalone_configs_infer_parent_without_prompting(self):
        for name in ('settings.standalone.example.json',):
            with self.subTest(config=name), \
                 patch.object(flash, 'idf_command', return_value=['python', 'idf.py']), \
                 patch.object(flash, 'prompt', side_effect=AssertionError('Unexpected prompt')):
                status, output = self.run_main('--config', str(ROOT/'config'/name),
                                               '--port', '/dev/ttyACM1', '--dry-run')
                self.assertEqual(status, 0, output)
                self.assertIn('Device: parent / ESP32-C5', output)
                self.assertIn('-DWIKNOW_ROLE=parent', output)

    def test_standalone_explicit_incompatible_roles_are_rejected(self):
        for role in ('universal', 'child', 'follower'):
            with self.subTest(role=role), patch.object(flash.subprocess, 'run') as execute:
                status, output = self.run_main(
                    '--config', str(ROOT/'config/settings.standalone.example.json'),
                    '--role', role, '--dry-run')
                self.assertEqual(status, 1, output)
                self.assertIn('board profiles require a manual role' if role == 'universal'
                              else 'standalone mode requires the parent role', output)
                execute.assert_not_called()

    def test_port_selection_and_rescan(self):
        with patch.object(flash, 'serial_ports', side_effect=[[], [('COM5', 'ESP32')]]), \
             patch.object(flash, 'prompt', side_effect=['r', '1']), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(flash.select_port(), 'COM5')

    def test_sdk_version_check(self):
        for version in ('ESP-IDF v6.1\n', 'ESP-IDF v6.1.0\n'):
            with patch.object(flash.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, version, '')):
                flash.check_idf(['python', 'idf.py'])
        for version in ('ESP-IDF v5.5.5', 'ESP-IDF v6.0', 'ESP-IDF v6.1.1', 'ESP-IDF v6.10',
                        'ESP-IDF v6.1-dirty', 'ESP-IDF v6.1-rc1', 'ESP-IDF v6.1-dev',
                        'ESP-IDF v6.1-1-g1234567', 'ESP-IDF v6.1.0-dirty', 'ESP-IDF v6.1 garbage'):
            with self.subTest(version=version), patch.object(
                    flash.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, version, '')):
                with self.assertRaises(ValueError):
                    flash.check_idf(['python', 'idf.py'])

    def test_wrong_sdk_never_builds_or_flashes(self):
        with patch.object(flash, 'idf_command', return_value=['python', 'idf.py']), \
             patch.object(flash.subprocess, 'run',
                          return_value=subprocess.CompletedProcess([], 0, 'ESP-IDF v5.5.5\n', '')) as execute:
            status, output = self.run_main('--port', 'COM7')
        self.assertEqual(status, 1, output)
        self.assertIn('Expected ESP-IDF v6.1', output)
        self.assertEqual(execute.call_count, 1)
        self.assertEqual(execute.call_args.args[0][-1], '--version')

    @unittest.skipUnless(shutil.which('cmake'), 'CMake is required')
    def test_direct_firmware_build_enforces_sdk_release(self):
        # Supply SDK metadata and project() / build-property stubs. The real
        # firmware entrypoint must reject wrong numeric versions before project
        # initialization, then reject prereleases and modified SDK revisions.
        cases = [(version, 'v' + version, False)
                 for version in ('5.5.5', '6.0.0', '6.1.1', '6.10.0')]
        cases += [('6.1.0', release, True) for release in ('v6.1', 'v6.1.0')]
        cases += [('6.1.0', release, False) for release in (
            'v6.1-dirty', 'v6.1.0-dirty', 'v6.1-dev', 'v6.1-rc1',
            'v6.1-1-g1234567', '', 'v6.10')]
        with tempfile.TemporaryDirectory() as folder:
            sdk = pathlib.Path(folder)
            project = sdk/'tools/cmake/project.cmake'
            project.parent.mkdir(parents=True)
            for version, release, accepted in cases:
                with self.subTest(version=version, release=release):
                    major, minor, patch_version = version.split('.')
                    project.write_text(
                        f'set(IDF_VERSION_MAJOR {major})\n'
                        f'set(IDF_VERSION_MINOR {minor})\n'
                        f'set(IDF_VERSION_PATCH {patch_version})\n'
                        'macro(project)\nmessage(STATUS "Firmware project initialized")\nendmacro()\n'
                        'function(idf_build_get_property output property)\n'
                        'if(NOT property STREQUAL "IDF_VER")\n'
                        'message(FATAL_ERROR "Unexpected SDK property")\nendif()\n'
                        f'set(${{output}} "{release}" PARENT_SCOPE)\nendfunction()\n')
                    result = subprocess.run(
                        ['cmake', '-P', str(ROOT/'firmware/CMakeLists.txt')],
                        env={**os.environ, 'IDF_PATH': str(sdk)},
                        capture_output=True, text=True)
                    if accepted:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn('Firmware project initialized', result.stdout)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        if version != '6.1.0':
                            self.assertIn('requires ESP-IDF v6.1.0', result.stderr)
                            self.assertNotIn('Firmware project initialized', result.stdout)
                        else:
                            self.assertIn('requires the ESP-IDF v6.1 release', result.stderr)

    def test_missing_sdk_actionable_error(self):
        with patch.dict(flash.os.environ, {}, clear=True), patch.object(flash.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(ValueError, 'ESP-IDF is not active'):
                flash.idf_command()
            self.assertEqual(flash.idf_command(dry_run=True)[-1], 'idf.py')

    def test_paths_with_spaces_remain_separate_arguments(self):
        commands = flash.commands(['python', '/sdk with spaces/idf.py'], 'parent',
                                  pathlib.Path('/config with spaces/settings.json'), 'COM7', 460800, False, False)
        self.assertEqual(commands[0][1], '/sdk with spaces/idf.py')
        self.assertIn('-DWIKNOW_CONFIG=/config with spaces/settings.json', commands[0])


if __name__ == '__main__':
    unittest.main()
