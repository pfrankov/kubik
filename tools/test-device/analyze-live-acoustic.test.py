"""Acceptance contrasts for measured-path speech preservation, without any API."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import numpy as np

spec = importlib.util.spec_from_file_location('acoustic', Path(__file__).with_name('analyze-live-acoustic.py'))
acoustic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acoustic)
RATE = acoustic.RATE


def processed(signal):
    return np.pad(signal, (acoustic.CLEAN_DELAY_SAMPLES, 0))[:len(signal)]


class MeasuredPath(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rng = np.random.default_rng(410)
        time = np.arange(5 * RATE) / RATE
        cls.source = np.zeros(len(time))
        for i, frequency in enumerate((170, 430, 780, 1250, 2100, 3300, 4900)):
            steps = rng.uniform(.05, 1, 26)
            amplitude = np.interp(time, np.linspace(0, 5, len(steps)), steps)
            cls.source += 900 * amplitude * np.sin(2 * np.pi * frequency * time + i)
        # Separate speaker/microphone phase response: envelope stays, full-wave phase does not.
        spectrum = np.fft.rfft(cls.source) * -1j
        spectrum[0] = spectrum[-1] = 0
        cls.physical = np.fft.irfft(spectrum, len(cls.source)) * .15
        cls.near_lag = 7680
        cls.near = np.pad(cls.physical, (cls.near_lag, 18000))
        cls.double_lag = 5280
        cls.spoken = np.pad(cls.physical, (cls.double_lag, 10000))
        cls.far = 900 * np.sin(2 * np.pi * 220 * np.arange(len(cls.spoken)) / RATE)
        cls.raw = cls.spoken + cls.far

    def test_phase_response_is_calibrated_without_weakening_preservation(self):
        template, lag = acoustic.calibrate(self.near, self.source)
        self.assertEqual(len(template), len(self.source))
        self.assertLess(abs(np.corrcoef(self.physical, self.source)[0, 1]), .01)
        acoustic.verify('near', self.near, processed(self.near * .9), np.zeros(len(self.near)), template, lag)
        acoustic.verify('double', self.raw, processed(self.spoken * .9 + self.far * .02), self.far, template)
        with self.assertRaisesRegex(AssertionError, 'suppressed'):
            acoustic.verify('double', self.raw, processed(self.spoken * .1 + self.far * .02), self.far, template)

    def test_complete_phrases_after_one_second_are_found_without_relaxing_confidence(self):
        lag = 37920  # Recorded calibration phrase offset: 1.58 seconds.
        near = np.pad(self.physical, (lag, 18000))
        template, found = acoustic.calibrate(near, self.source)
        self.assertEqual(found, lag)
        gain, correlation, found = acoustic.projection(near, self.physical)
        self.assertEqual(found, lag)
        self.assertAlmostEqual(gain, 1)
        self.assertAlmostEqual(correlation, 1)
        acoustic.verify('near', near, processed(near * .9), np.zeros(len(near)), template, lag)
        for delay in (0, 960):
            with self.subTest(delay=delay), self.assertRaises(AssertionError):
                acoustic.verify('near', near, np.pad(near, (delay, 0))[:len(near)],
                                np.zeros(len(near)), template, lag)

    def test_capture_bound_is_checked_before_search_or_reading(self):
        oversized = np.zeros(acoustic.MAX_CAPTURE_SAMPLES + 1)
        for check in (acoustic.calibrate, acoustic.projection):
            with self.subTest(check=check.__name__), self.assertRaisesRegex(AssertionError, '30 second'):
                check(oversized, self.source)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.pcm'
            with path.open('wb') as stream:
                stream.truncate(acoustic.MAX_CAPTURE_SAMPLES * 2 + 2)
            with self.assertRaisesRegex(AssertionError, '30 second'):
                acoustic.read_capture(path)
            path.write_bytes(b'\0')
            with self.assertRaisesRegex(AssertionError, 'incomplete PCM16'):
                acoustic.read_capture(path)

    def test_failed_calibration_reports_measured_scores_and_original_thresholds(self):
        noise = np.random.default_rng(23).normal(0, 100, len(self.near) + RATE * 2)
        with self.assertRaisesRegex(AssertionError, r'envelope=.*required >0.800.*bands=.*required >0.650'):
            acoustic.calibrate(noise, self.source)

    def test_only_the_measured_preprocessor_delay_is_accepted(self):
        for delay in (0, 960):
            clean = np.pad(self.spoken, (delay, 0))[:len(self.spoken)]
            with self.subTest(delay=delay), self.assertRaisesRegex(AssertionError, 'suppressed'):
                acoustic.verify('double', self.raw, clean, self.far, self.physical)

    def test_unrelated_audio_and_silent_calibration_are_rejected(self):
        noise = np.random.default_rng(58).normal(0, 100, len(self.near))
        with self.assertRaisesRegex(AssertionError, 'absent'):
            acoustic.calibrate(noise, self.source)
        with np.errstate(invalid='ignore', divide='ignore'):
            with self.assertRaises(AssertionError): acoustic.calibrate(np.zeros(len(self.near)), self.source)

    def test_broadband_unrelated_voice_does_not_replace_near_speech(self):
        time = np.arange(len(self.spoken)) / RATE
        far = np.zeros(len(time))
        rng = np.random.default_rng(802)
        for frequency in (260, 650, 910, 1800, 2900):
            amplitude = np.interp(time, np.linspace(0, 6, 31), rng.uniform(.05, 1, 31))
            far += 600 * amplitude * np.sin(2 * np.pi * frequency * time)
        acoustic.verify('double', self.spoken + far, processed(self.spoken * .9 + far * .01), far, self.physical)
        with self.assertRaisesRegex(AssertionError, 'confidence'):
            acoustic.verify('double', far, far * .01, far, self.physical)

    def test_missing_overlap_truncation_and_shifted_clean_are_rejected(self):
        for kind, clean, reference in (
            ('overlap', self.spoken, np.zeros(len(self.raw))),
            ('shift', np.roll(self.spoken, 6000), self.far),
        ):
            with self.subTest(kind=kind), self.assertRaises(AssertionError):
                acoustic.verify('double', self.raw, clean, reference, self.physical)
        with self.assertRaisesRegex(AssertionError, 'Truncated'):
            acoustic.projection(self.spoken[:1000], self.physical, self.double_lag)
        with self.assertRaisesRegex(AssertionError, 'confidence'):
            acoustic.verify('double', self.far, self.far * .01, self.far, self.physical)


if __name__ == '__main__': unittest.main()
