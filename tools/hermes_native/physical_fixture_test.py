"""An answer's PCM duration follows its exact text, never the number of delivered blocks."""
import os
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from device_voice import Observations, PhysicalConnection


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


class PhysicalEchoGateTests(unittest.TestCase):
    def measure(self, reference, clean, evidence):
        observation = Observations.__new__(Observations)
        pcm = lambda level: struct.pack('<h', level) * 960
        observation.frames = [(pcm(1000), pcm(reference), pcm(clean))] * 125
        return observation.acoustic(evidence)

    def test_present_reference_with_bypassed_cleaning_fails_and_keeps_measurements(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            with self.assertRaisesRegex(AssertionError, 'echo suppression'):
                self.measure(1000, 1000, evidence)
            self.assertEqual(json.loads((evidence / 'acoustic.json').read_text())['erle_db'], 0)
            self.assertTrue(all((evidence / name).is_file() for name in ('mic.pcm', 'reference.pcm', 'clean.pcm')))

    def test_present_reference_and_twenty_db_attenuation_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.measure(1000, 100, Path(directory))
            self.assertAlmostEqual(result['erle_db'], 20)
            self.assertEqual(result['reference_rms'], 1000)

    def test_missing_reference_fails_even_when_output_is_attenuated(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(AssertionError, 'reference is absent'):
                self.measure(0, 100, Path(directory))


if __name__ == '__main__':
    unittest.main()
