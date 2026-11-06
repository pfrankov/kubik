"""Hermes's official probe policy must stop pending source-update work before any mutation."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


class RuntimeGuardTests(unittest.TestCase):
    def test_probe_switch_blocks_source_inspection_and_contrast_reaches_it(self):
        from hermes_cli.venv_sync import prepare_launch
        with tempfile.TemporaryDirectory(prefix='kubik-guard-') as directory:
            root = Path(directory)
            (root / '.git').mkdir()
            (root / 'pyproject.toml').write_text('[project]\nname = "fixture"\n')
            (root / 'install-stamp.json').write_text('{"updateMechanism":"self"}')
            (root / '.source-completion-pending').write_text('pending fixture tail\n')
            before = {path.name: path.read_bytes() for path in root.iterdir() if path.is_file()}
            with patch('hermes_cli.steward.read_install_stamp', side_effect=RuntimeError('source inspection')) as read:
                with patch.dict(os.environ, {'HERMES_DISABLE_LAZY_INSTALLS': '1'}):
                    self.assertIsNone(prepare_launch(root, ['gateway', 'run']))
                read.assert_not_called()
                with patch.dict(os.environ, {'HERMES_DISABLE_LAZY_INSTALLS': '0'}):
                    with self.assertRaisesRegex(RuntimeError, 'source inspection'):
                        prepare_launch(root, ['gateway', 'run'])
                read.assert_called_once()
            self.assertEqual(before, {path.name: path.read_bytes() for path in root.iterdir() if path.is_file()})


if __name__ == '__main__':
    unittest.main()
