"""Filesystem contracts of the installer; real-runtime coverage lives in the Hermes integration lane."""
import importlib.util
import io
from pathlib import Path
import shlex
import stat
import tempfile
import unittest
from unittest.mock import patch

from ruamel.yaml import YAML

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('kubik_installer_contract', ROOT / 'tools/install-hermes.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallerContractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        # Match install(), including macOS' /var -> /private/var canonicalization.
        self.home = Path(self.temporary.name).resolve()
        self.config = self.home / 'config.yaml'
        self.source = ROOT / 'hermes-kubik'
        self.original = ('# Keep my providers and comments\nmodel: "existing-model"\n'
                         'plugins:\n  enabled: [another-plugin]\n  disabled: [kubik-platform, unrelated]\n'
                         'platforms:\n  telegram:\n    enabled: true\n'
                         'stt:\n  provider: openai\n  openai:\n    api_key: user-secret\n')
        self.config.write_text(self.original)

    def read(self):
        return YAML().load(self.config.read_text())

    def test_idempotent_upgrade_preserves_provider_settings_comments_and_custom_listener(self):
        installer.install(self.source, self.home, 28793, '127.0.0.1')
        first = self.config.read_bytes()
        installer.install(self.source, self.home)
        self.assertEqual(self.config.read_bytes(), first)
        saved = self.read()
        self.assertEqual(saved['platforms']['kubik']['extra'], {'port': 28793, 'bind': '127.0.0.1'})
        self.assertEqual(saved['plugins']['enabled'], ['another-plugin', 'kubik-platform'])
        self.assertEqual(saved['plugins']['disabled'], ['unrelated'])
        self.assertEqual(saved['stt']['openai']['api_key'], 'user-secret')
        self.assertIn('# Keep my providers and comments', first.decode())
        self.assertIn('model: "existing-model"', first.decode())
        self.assertEqual((self.home / 'config.before-kubik.yaml').read_text(), self.original)
        self.assertEqual(len(list((self.home / 'plugins').rglob('plugin.yaml'))), 1)
        for name in ('config.yaml', 'config.before-kubik.yaml', '.kubik-install.lock'):
            self.assertEqual(stat.S_IMODE((self.home / name).stat().st_mode), 0o600)

    def test_check_is_read_only(self):
        before = {path.relative_to(self.home): (path.read_bytes(), path.stat().st_mode)
                  for path in self.home.rglob('*') if path.is_file()}
        installer.install(self.source, self.home, check_only=True)
        after = {path.relative_to(self.home): (path.read_bytes(), path.stat().st_mode)
                 for path in self.home.rglob('*') if path.is_file()}
        self.assertEqual(after, before)
        self.assertFalse((self.home / 'plugins').exists())

    def test_invalid_configuration_does_not_install_or_weaken_isolation(self):
        cases = ['[]\n', 'plugins: []\n', 'plugins: {enabled: all}\n',
                 'plugins: {isolation: host}\n', 'platforms: {kubik: []}\n',
                 'platforms: {kubik: {extra: {port: true}}}\n',
                 'platforms: {kubik: {extra: {bind: localhost}}}\n', 'plugins: [\n']
        for text in cases:
            with self.subTest(config=text):
                self.config.write_text(text)
                with self.assertRaises(ValueError):
                    installer.install(self.source, self.home)
                self.assertEqual(self.config.read_text(), text)
                self.assertFalse((self.home / 'plugins').exists())
                self.assertFalse((self.home / 'config.before-kubik.yaml').exists())

    def test_symlink_configuration_and_plugin_directory_are_rejected(self):
        target = self.home / 'other.yaml'
        self.config.rename(target)
        self.config.symlink_to(target)
        with self.assertRaises(ValueError):
            installer.install(self.source, self.home)
        self.assertEqual(target.read_text(), self.original)
        self.config.unlink()
        target.rename(self.config)
        (self.home / 'plugins').symlink_to(self.home, target_is_directory=True)
        with self.assertRaises(ValueError):
            installer.install(self.source, self.home)
        self.assertEqual(self.config.read_text(), self.original)

    def test_configuration_write_failure_restores_the_previous_plugin(self):
        destination = self.home / 'plugins/kubik'
        destination.mkdir(parents=True)
        (destination / 'original.py').write_text('previous implementation\n')
        real_link = installer.os.link

        def fail_config(path, target, **kwargs):
            if Path(target) == self.config and Path(path).name == 'config.yaml':
                raise OSError('simulated configuration commit failure')
            return real_link(path, target, **kwargs)

        with patch.object(installer.os, 'link', fail_config):
            with self.assertRaises(OSError):
                installer.install(self.source, self.home)
        self.assertEqual(self.config.read_text(), self.original)
        self.assertEqual([path.name for path in destination.iterdir()], ['original.py'])
        self.assertEqual((destination / 'original.py').read_text(), 'previous implementation\n')

    def test_conflicting_installer_does_not_modify_configuration(self):
        with installer.installation_lock(self.home):
            with self.assertRaises(OSError):
                installer.install(self.source, self.home)
        self.assertEqual(self.config.read_text(), self.original)
        self.assertFalse((self.home / 'plugins').exists())

    def test_changed_configuration_is_not_overwritten_during_promotion(self):
        staged, destination, previous = (self.home / name for name in ('staged', 'installed', 'previous'))
        staged.mkdir()
        prepared = self.home / 'prepared.yaml'
        prepared.write_text('new: config\n')
        self.config.write_text('changed: externally\n')
        with self.assertRaises(ValueError):
            installer.promote(staged, destination, self.config, prepared, self.original.encode(), previous)
        self.assertEqual(self.config.read_text(), 'changed: externally\n')
        self.assertTrue(staged.exists())
        self.assertFalse(destination.exists())

    def test_configuration_replaced_after_validation_is_not_overwritten(self):
        destination = self.home / 'plugins/kubik'
        destination.parent.mkdir(parents=True)
        staged = self.home / 'staged'
        staged.mkdir()
        prepared = self.home / 'prepared.yaml'
        prepared.write_text('new: config\n')
        previous = self.home / 'previous'
        actual_rename = Path.rename

        def replace_after_plugin_promotion(path, target):
            result = actual_rename(path, target)
            if Path(path) == staged:
                self.config.write_text('changed: after-validation\n')
            return result

        with patch.object(Path, 'rename', replace_after_plugin_promotion):
            with self.assertRaisesRegex(ValueError, 'changed during installation'):
                installer.promote(staged, destination, self.config, prepared,
                                  self.original.encode(), previous)
        self.assertEqual(self.config.read_text(), 'changed: after-validation\n')
        self.assertFalse(destination.exists())

    def test_configuration_created_during_commit_keeps_original_backup(self):
        actual_link = installer.os.link

        def create_before_commit(path, target, **kwargs):
            if (Path(target) == self.config and Path(path).name == 'config.yaml'
                    and Path(path).parent.name.startswith('.kubik-config-')):
                self.config.write_text('changed: during-commit\n')
            return actual_link(path, target, **kwargs)

        with patch.object(installer.os, 'link', create_before_commit):
            with self.assertRaises(FileExistsError):
                installer.install(self.source, self.home)
        self.assertEqual(self.config.read_text(), 'changed: during-commit\n')
        self.assertEqual((self.home / 'config.before-kubik.yaml').read_text(), self.original)
        self.assertFalse((self.home / 'plugins/kubik').exists())

    def test_pairing_command_is_shell_safe(self):
        home = self.home / 'Hermes home; not-a-command'
        home.mkdir()
        (home / 'config.yaml').write_text(self.original)
        output = io.StringIO()
        with patch('sys.stdout', output):
            installer.install(self.source, home)
        command = output.getvalue().split('Approve the code on Kubik: ', 1)[1].strip()
        self.assertEqual(shlex.split(command),
                         [installer.sys.executable, str(home / 'plugins/kubik/pair.py'), 'CODE'])

    def test_failed_install_keeps_backup_only_when_the_original_cannot_be_restored(self):
        backup = self.home / 'config.before-kubik.yaml'
        actual_write = installer.private_write

        def change_during_staging(path, data):
            actual_write(path, data)
            if path.name == 'config.yaml':
                self.config.write_text('model: changed-by-another-process\n')

        for existing in (False, True):
            with self.subTest(existing_backup=existing):
                self.config.write_text(self.original)
                if existing:
                    backup.write_text('first: successful-install\n')
                with patch.object(installer, 'private_write', change_during_staging):
                    with self.assertRaisesRegex(ValueError, 'changed during installation'):
                        installer.install(self.source, self.home)
                self.assertEqual(self.config.read_text(), 'model: changed-by-another-process\n')
                self.assertTrue(backup.exists())
                if existing:
                    self.assertEqual(backup.read_text(), 'first: successful-install\n')
                else:
                    self.assertEqual(backup.read_text(), self.original)
                self.assertFalse((self.home / 'plugins/kubik').exists())
                self.assertFalse(list((self.home / 'plugins').glob('.kubik-install-*')))

    def test_unsupported_interpreter_fails_before_any_dependency_probe(self):
        with patch.object(installer.sys, 'version_info', (3, 11)):
            with self.assertRaisesRegex(ValueError, 'Python 3.14'):
                installer.require_runtime()


if __name__ == '__main__':
    unittest.main()
