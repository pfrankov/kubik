"""Process-death recovery at real filesystem commit boundaries; no Hermes/provider calls."""
import json
from pathlib import Path
import stat
import subprocess
import sys
import unittest
from unittest.mock import patch

import hermes_installer_test as shared

installer = shared.installer
CHILD = r'''
import importlib.util, os
from pathlib import Path
import sys
spec = importlib.util.spec_from_file_location('installer', sys.argv[1])
i = importlib.util.module_from_spec(spec)
spec.loader.exec_module(i)
home, source, point = Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
config = home / 'config.yaml'
rename, link, transaction, remove = Path.rename, i.os.link, i.write_transaction, i.shutil.rmtree

def crash():
    os._exit(91)

def renamed(path, target):
    result = rename(path, target)
    if point == 'plugin_removed' and Path(target).name == 'previous': crash()
    if point == 'plugin_new' and Path(path).name == 'plugin': crash()
    if point in ('config_moved', 'external_config') and Path(path) == config:
        if point == 'external_config': config.write_text('model: changed-externally\n')
        crash()
    return result

def linked(path, target, **kwargs):
    result = link(path, target, **kwargs)
    if point == 'config_linked' and Path(target) == config and Path(path).name == 'config.yaml': crash()
    return result

def journal(home, state):
    transaction(home, state)
    if point == state['phase']: crash()

def removed(path, *args, **kwargs):
    result = remove(path, *args, **kwargs)
    if point == 'cleanup' and Path(path).name.startswith('.kubik-plugin-'): crash()
    return result

Path.rename, i.os.link, i.write_transaction, i.shutil.rmtree = renamed, linked, journal, removed
i.install(source, home)
raise AssertionError('Crash boundary was not reached')
'''


class RecoveryContractTest(unittest.TestCase):
    setUp = shared.InstallerContractTest.setUp

    def crash(self, point):
        result = subprocess.run([sys.executable, '-c', CHILD,
                                 str(shared.ROOT / 'tools/install-hermes.py'),
                                 str(self.home), str(self.source), point], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 91, result.stdout + result.stderr)
        journal = self.home / '.kubik-install.json'
        self.assertTrue(journal.exists())
        self.assertEqual(stat.S_IMODE(journal.stat().st_mode), 0o600)

    def recover(self):
        with installer.installation_lock(self.home):
            installer.recover_installation(self.home)

    def old_plugin(self):
        old = self.home / 'plugins/kubik'
        old.mkdir(parents=True)
        (old / 'original.py').write_text('previous implementation\n')
        return old

    def assert_clean(self):
        self.assertFalse((self.home / '.kubik-install.json').exists())
        self.assertFalse(list(self.home.glob('.kubik-config-*')))
        self.assertFalse(list((self.home / 'plugins').glob('.kubik-plugin-*')))

    def test_process_death_recovers_every_commit_boundary(self):
        for upgrade in (False, True):
            points = ['staging', 'prepared', 'plugin_new', 'config_moved', 'config_linked', 'committed', 'cleanup']
            if upgrade:
                points.append('plugin_removed')
            for point in points:
                with self.subTest(upgrade=upgrade, point=point):
                    # Each boundary has its own home and independent original state.
                    self.setUp()
                    destination = self.old_plugin() if upgrade else self.home / 'plugins/kubik'
                    self.crash(point)
                    self.recover()
                    committed = point in ('config_linked', 'committed', 'cleanup')
                    if committed:
                        self.assertIn('kubik', shared.YAML().load(self.config.read_text())['plugins']['enabled'])
                        self.assertTrue((destination / 'adapter.py').exists())
                    else:
                        self.assertEqual(self.config.read_text(), self.original)
                        if upgrade:
                            self.assertEqual((destination / 'original.py').read_text(), 'previous implementation\n')
                        else:
                            self.assertFalse(destination.exists())
                    self.assert_clean()
                    installer.install(self.source, self.home)
                    saved = shared.YAML().load(self.config.read_text())
                    self.assertEqual(saved['model'], 'existing-model')
                    self.assertEqual(saved['stt']['openai']['api_key'], 'user-secret')
                    self.assert_clean()

    def test_external_config_in_the_commit_gap_is_preserved_with_its_predecessor(self):
        destination = self.old_plugin()
        (self.home / 'config.before-kubik.yaml').write_text('model: first-backup\n')
        self.crash('external_config')
        self.recover()
        self.assertEqual(self.config.read_text(), 'model: changed-externally\n')
        self.assertEqual((self.home / 'config.before-kubik-conflict.yaml').read_text(), self.original)
        self.assertTrue((destination / 'original.py').exists())
        self.assert_clean()
        installer.install(self.source, self.home)
        self.assertEqual(shared.YAML().load(self.config.read_text())['model'], 'changed-externally')

    def test_unrecognized_plugin_replacement_is_not_deleted_by_recovery(self):
        destination = self.old_plugin()
        self.crash('plugin_new')
        displaced = self.home / 'external-displaced'
        destination.rename(displaced)
        destination.mkdir()
        (destination / 'unrelated.py').write_text('external plugin\n')
        with self.assertRaisesRegex(ValueError, 'plugin changed'):
            self.recover()
        self.assertEqual((destination / 'unrelated.py').read_text(), 'external plugin\n')
        self.assertTrue((self.home / '.kubik-install.json').exists())
        state = json.loads((self.home / '.kubik-install.json').read_text())
        self.assertTrue((self.home / 'plugins' / state['plugin'] / 'previous/original.py').exists())

    def test_check_reports_interruption_without_repairing_or_writing(self):
        self.crash('config_moved')
        before = {str(p.relative_to(self.home)): p.read_bytes() for p in self.home.rglob('*') if p.is_file()}
        with self.assertRaisesRegex(ValueError, 'without --check to recover'):
            installer.install(self.source, self.home, check_only=True)
        after = {str(p.relative_to(self.home)): p.read_bytes() for p in self.home.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
        self.recover()
        self.assertEqual(self.config.read_text(), self.original)
        self.assert_clean()

    def test_caught_failure_also_preserves_an_unrecognized_plugin_replacement(self):
        destination = self.old_plugin()
        actual_link = installer.os.link

        def replace_before_commit(path, target, **kwargs):
            if Path(target) == self.config and Path(path).name == 'config.yaml':
                destination.rename(self.home / 'displaced-candidate')
                destination.mkdir()
                (destination / 'unrelated.py').write_text('external plugin\n')
                self.config.write_text('model: external-conflict\n')
            return actual_link(path, target, **kwargs)

        with patch.object(installer.os, 'link', replace_before_commit):
            with self.assertRaisesRegex(ValueError, 'plugin changed'):
                installer.install(self.source, self.home)
        self.assertEqual((destination / 'unrelated.py').read_text(), 'external plugin\n')
        self.assertEqual(self.config.read_text(), 'model: external-conflict\n')
        self.assertTrue((self.home / '.kubik-install.json').exists())

    def test_plugin_changed_during_staging_is_not_overwritten(self):
        for upgrade in (False, True):
            with self.subTest(upgrade=upgrade):
                self.setUp()
                destination = self.old_plugin() if upgrade else self.home / 'plugins/kubik'
                write = installer.private_write

                def replace_during_staging(path, data):
                    write(path, data)
                    if path.name == 'config.yaml':
                        if destination.exists():
                            destination.rename(self.home / 'original-plugin')
                        destination.mkdir()
                        (destination / 'unrelated.py').write_text('external plugin\n')

                with patch.object(installer, 'private_write', replace_during_staging):
                    with self.assertRaisesRegex(ValueError, 'plugin changed during staging'):
                        installer.install(self.source, self.home)
                self.assertEqual((destination / 'unrelated.py').read_text(), 'external plugin\n')
                self.assertEqual(self.config.read_text(), self.original)
                self.assert_clean()

    def test_replaced_prepared_plugin_is_not_promoted_or_deleted(self):
        destination = self.old_plugin()
        write = installer.write_transaction

        def replace_prepared(home, state):
            write(home, state)
            if state['phase'] == 'prepared':
                staged = home / 'plugins' / state['plugin'] / 'plugin'
                staged.rename(home / 'displaced-candidate')
                staged.mkdir()
                (staged / 'unrelated.py').write_text('external staged plugin\n')

        with patch.object(installer, 'write_transaction', replace_prepared):
            with self.assertRaisesRegex(ValueError, 'Staged Kubik plugin changed'):
                installer.install(self.source, self.home)
        self.assertTrue((destination / 'original.py').exists())
        self.assertEqual(self.config.read_text(), self.original)
        state = json.loads((self.home / '.kubik-install.json').read_text())
        self.assertEqual((self.home / 'plugins' / state['plugin'] / 'plugin/unrelated.py').read_text(),
                         'external staged plugin\n')


if __name__ == '__main__':
    unittest.main()
