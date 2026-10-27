"""Integration coverage for automatic regeneration from a custom rules directory."""
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cmake'), 'CMake is required')
class BuildTests(unittest.TestCase):
    def test_custom_rules_regenerate_on_edit_add_and_remove(self):
        with tempfile.TemporaryDirectory() as folder:
            source = pathlib.Path(folder) / 'source'
            source.mkdir()
            shutil.copy(ROOT / 'CMakeLists.txt', source)
            for name in ('components', 'tools', 'tests', 'config', 'firmware'):
                shutil.copytree(ROOT / name, source / name)
            rules = source / 'custom-rules'
            rules.mkdir()
            settings = json.loads((source / 'config/settings.json').read_text())
            settings['detection']['rules_directory'] = 'custom-rules'
            settings['sd']['model'] = 'Unicode 🔎; quote "; slash \\; NUL \x00; trigraph ??/'
            (source / 'config/settings.json').write_text(json.dumps(settings))
            document = json.loads((ROOT / 'rules/examples/lab.json').read_text())
            (rules / 'first.json').write_text(json.dumps(document))
            build = pathlib.Path(folder) / 'build'

            def run(*args):
                result = subprocess.run(args, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            def generated():
                run('cmake', '--build', str(build))
                return (build / 'generated/settings.hpp').read_text()

            def manifest_ids():
                document = json.loads((build / 'generated/settings.rules.json').read_text())
                return [rule['id'] for rule in document['rules']]

            run('cmake', '-S', str(source), '-B', str(build))
            self.assertIn('lab-test-beacon', generated())
            self.assertEqual(manifest_ids(), ['lab-test-beacon'])
            document['rules'][0]['id'] = 'edited-beacon'
            (rules / 'first.json').write_text(json.dumps(document))
            self.assertIn('edited-beacon', generated())
            self.assertEqual(manifest_ids(), ['edited-beacon'])
            document['rules'][0]['id'] = 'added-beacon'
            (rules / 'second.json').write_text(json.dumps(document))
            self.assertIn('added-beacon', generated())
            self.assertEqual(manifest_ids(), ['edited-beacon', 'added-beacon'])
            (rules / 'first.json').unlink()
            self.assertNotIn('edited-beacon', generated())
            self.assertEqual(manifest_ids(), ['added-beacon'])
