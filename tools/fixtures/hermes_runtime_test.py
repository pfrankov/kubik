"""Managed preparation keeps the complete plugin union and defers live code writes."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runtime = load('hermes_runtime', 'tools/hermes_runtime.py')
installer = load('hermes_prepare_installer', 'tools/install-hermes.py')


class ManagedPreparationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.home = self.root / 'hermes'
        self.home.mkdir()
        self.other = self.home / 'profiles/other'
        self.other.mkdir(parents=True)
        self.before = b'plugins:\n  enabled: [another, kubik]\n  disabled: []\nmodel: preserved\n'
        (self.home / 'config.yaml').write_bytes(self.before)
        (self.other / 'config.yaml').write_text('plugins: {enabled: [separate]}\n')
        self.old = self.home / 'plugins/kubik'
        self.old.mkdir(parents=True)
        (self.old / 'old.py').write_text('old code remains active')
        self.members = [self.home / 'plugins/another', self.old, self.other / 'plugins/separate']
        self.sync = Mock()
        self.discover = Mock(return_value=self.members)
        self.child = Mock(return_value=SimpleNamespace(returncode=0))
        pm = ModuleType('pm')
        pm.__path__ = []
        self.modules = {
            'hermes_runtime': runtime, 'pm': pm,
            'pm.paths': SimpleNamespace(repo_root=lambda: self.root / 'hermes-source'),
            'pm.client': SimpleNamespace(sync_venv=self.sync),
            'pm.environments': SimpleNamespace(selected_venv=lambda _root: self.root / 'selected',
                                               venv_command=lambda _root, venv: [str(venv / 'bin/python')]),
            'pm.plugin_inputs': SimpleNamespace(Members=lambda sources: SimpleNamespace(dirs=sources)),
            'pm.plugins_state': SimpleNamespace(dependency_homes=lambda: [self.home, self.other]),
            'pm.workspace': SimpleNamespace(enabled_member_dirs=self.discover),
        }
        self.addCleanup(patch.stopall)
        patch.dict(sys.modules, self.modules).start()
        patch.object(runtime.subprocess, 'run', self.child).start()

    def prepare(self):
        installer.prepare_install(ROOT / 'hermes-kubik', self.home, 28793, '127.0.0.1')

    def unchanged(self):
        self.assertEqual((self.home / 'config.yaml').read_bytes(), self.before)
        self.assertEqual((self.old / 'old.py').read_text(), 'old code remains active')
        self.assertFalse((self.home / 'config.before-kubik.yaml').exists())

    def test_candidate_includes_canonical_echo_and_every_profile_before_selected_install(self):
        def sync(**kwargs):
            self.unchanged()
            self.assertTrue(kwargs['explicit'])
            sources = kwargs['plugins'].dirs
            self.assertEqual(set(sources), set(self.members))
            candidate = sources[self.old]
            self.assertNotEqual(candidate, self.old)
            self.assertEqual((candidate / 'speex-aec.wasm').read_bytes(),
                             (ROOT / 'openclaw-kubik/src/vendor/speex-aec.wasm').read_bytes())
            self.assertIn('wasmtime==50.0.1', (candidate / 'requirements.txt').read_text())
            self.assertEqual(os.environ['HERMES_HOME'], str(self.home))
        self.sync.side_effect = sync
        with patch.dict(os.environ, {'HERMES_HOME': 'prior-profile'}):
            self.prepare()
            self.assertEqual(os.environ['HERMES_HOME'], 'prior-profile')
        self.unchanged()
        command = self.child.call_args.args[0]
        self.assertEqual(command[0], str(self.root / 'selected/bin/python'))
        self.assertNotIn('--prepare-runtime', command)
        self.assertEqual(command[-4:], ['--port', '28793', '--bind', '127.0.0.1'])
        self.assertEqual(self.discover.call_args.kwargs,
                         {'proposed_home': self.home, 'enabled': ['another', 'kubik'], 'disabled': []})

    def test_pm_failure_leaves_code_config_and_other_profiles_in_place(self):
        self.sync.side_effect = RuntimeError('resolution conflict')
        with self.assertRaisesRegex(ValueError, 'dependency preparation failed'):
            self.prepare()
        self.unchanged()
        self.child.assert_not_called()

    def test_concurrent_profile_edit_is_not_overwritten(self):
        self.sync.side_effect = lambda **_kwargs: (self.other / 'config.yaml').write_text('model: changed\n')
        with self.assertRaisesRegex(ValueError, 'profile selection changed'):
            self.prepare()
        self.unchanged()
        self.assertEqual((self.other / 'config.yaml').read_text(), 'model: changed\n')
        self.child.assert_not_called()

    def test_source_edit_or_failed_child_does_not_claim_installation(self):
        real_stage = installer.stage_plugin
        count = 0

        def changed_stage(source, staged, echo):
            nonlocal count
            real_stage(source, staged, echo)
            count += 1
            if count == 2:
                (staged / 'requirements.txt').write_text('changed during sync\n')
        with patch.object(installer, 'stage_plugin', changed_stage):
            with self.assertRaisesRegex(ValueError, 'source changed'):
                self.prepare()
        self.child.assert_not_called()
        self.child.return_value.returncode = 7
        with self.assertRaisesRegex(ValueError, 'installation exited 7'):
            self.prepare()
        self.unchanged()

    def test_invalid_candidate_does_not_start_pm_or_change_the_install(self):
        with self.assertRaisesRegex(ValueError, 'echo artifact'):
            installer.prepare_install(self.root / 'absent', self.home, None, None)
        self.sync.assert_not_called()
        self.child.assert_not_called()
        self.unchanged()

    def test_atomic_installer_timeout_reports_unfinished_installation(self):
        self.child.side_effect = runtime.subprocess.TimeoutExpired('installer', 120)
        with self.assertRaisesRegex(ValueError, 'atomic installation exceeded 120 seconds'):
            self.prepare()
        self.unchanged()


if __name__ == '__main__':
    unittest.main()
