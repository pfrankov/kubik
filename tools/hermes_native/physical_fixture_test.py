"""An answer's PCM duration follows its exact text, never the number of delivered blocks."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from device_voice import PhysicalConnection


class PhysicalFixtureTests(unittest.TestCase):
    def test_whole_and_split_answer_generate_identical_eight_second_pcm(self):
        whole, split = PhysicalConnection('realtime', None), PhysicalConnection('realtime', None)
        expected = whole.speech_slice(whole.expected_text)
        chunks = (split.expected_text[:7], split.expected_text[7:19], split.expected_text[19:])
        actual = b''.join(split.speech_slice(chunk) for chunk in chunks)
        self.assertEqual(actual, expected)
        self.assertEqual(len(actual), 8000 * 48)
        self.assertEqual(split.spoken_text, split.expected_text)
        self.assertEqual(split.generated_pcm_bytes, 8000 * 48)

    def test_duplicate_answer_and_unrelated_notice_fail_instead_of_expanding_audio(self):
        connection = PhysicalConnection('realtime', None)
        with self.assertRaisesRegex(AssertionError, 'Unexpected'):
            connection.speech_slice('No home channel is set')
        self.assertEqual(connection.generated_pcm_bytes, 0)
        connection.speech_slice(connection.expected_text)
        with self.assertRaisesRegex(AssertionError, 'duplicated'):
            connection.speech_slice(connection.expected_text)
        self.assertEqual(connection.generated_pcm_bytes, 8000 * 48)

    def test_physical_fixture_configures_private_home_without_changing_production(self):
        from contract import configure
        from ruamel.yaml import YAML
        with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ):
            home = Path(temporary)
            configure(home, 12345, 12346, home_identity='kubik-test:' + 'a' * 32)
            data = YAML(typ='safe').load((home / 'config.yaml').read_text())
            self.assertEqual(data['platforms']['kubik']['home_channel'], {
                'platform': 'kubik', 'chat_id': 'kubik-test:' + 'a' * 32, 'name': 'Kubik fixture'})
            self.assertEqual(data['model']['default'], 'kubik-test')


if __name__ == '__main__':
    unittest.main()
