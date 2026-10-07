"""Hermes approval and state-file regression tests."""
import json
from pathlib import Path
from unittest.mock import patch

from hermes_test_support import ProtocolFixture, Security, atomic_json


class SecurityState(ProtocolFixture):
    def test_restored_state_is_private_without_changing_identity(self):
        identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [identity])
        fingerprint = self.store.bind
        self.store.home.chmod(0o755)
        for path in self.store.home.iterdir():
            path.chmod(0o644)
        restored = Security(self.temp.name)
        restored.tls()
        self.assertEqual(restored.bind, fingerprint)
        self.assertTrue(restored.approved(identity))
        self.assertEqual(restored.home.stat().st_mode & 0o777, 0o700)
        for path in restored.home.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_atomic_state_ignores_stale_temp_and_cleans_failed_writes(self):
        path = self.store.allow_path
        sentinel = self.store.home / 'sentinel'
        sentinel.write_text('unchanged')
        path.with_suffix('.tmp').symlink_to(sentinel)
        atomic_json(path, ['approved'])
        self.assertEqual(sentinel.read_text(), 'unchanged')
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        before = set(self.store.home.iterdir())
        with patch('hermes_kubik.security.os.replace', side_effect=OSError('write failed')):
            with self.assertRaises(OSError):
                atomic_json(path, ['replacement'])
        self.assertEqual(json.loads(path.read_text()), ['approved'])
        self.assertEqual(set(self.store.home.iterdir()), before)

    def test_restored_state_rejects_symlinks_without_changing_target(self):
        target = self.store.home / 'external'
        target.write_text('keep')
        target.chmod(0o644)
        self.store.allow_path.symlink_to(target)
        with self.assertRaises(ValueError):
            Security(self.temp.name)
        self.assertEqual(target.read_text(), 'keep')
        self.assertEqual(target.stat().st_mode & 0o777, 0o644)

    def test_state_directory_symlink_does_not_change_target_permissions(self):
        home = Path(self.temp.name) / 'other-home'
        home.mkdir()
        target = Path(self.temp.name) / 'external'
        target.mkdir()
        target.chmod(0o755)
        sentinel = target / 'approved.json'
        sentinel.write_text('[]')
        sentinel.chmod(0o644)
        (home / 'kubik').symlink_to(target, target_is_directory=True)
        with self.assertRaises(ValueError):
            Security(home)
        self.assertEqual(target.stat().st_mode & 0o777, 0o755)
        self.assertEqual(sentinel.stat().st_mode & 0o777, 0o644)
        self.assertEqual(sentinel.read_text(), '[]')
