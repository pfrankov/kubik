"""Acceptance contrasts for measured-path speech preservation, without any API."""
import importlib.util
from pathlib import Path
import unittest
import numpy as np

spec = importlib.util.spec_from_file_location('acoustic', Path(__file__).with_name('analyze-live-acoustic.py'))
acoustic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acoustic)
RATE = acoustic.RATE


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
        acoustic.verify('near', self.near, self.near * .9, np.zeros(len(self.near)), template, lag)
        acoustic.verify('double', self.raw, self.spoken * .9 + self.far * .02, self.far, template)
        with self.assertRaisesRegex(AssertionError, 'suppressed'):
            acoustic.verify('double', self.raw, self.spoken * .1 + self.far * .02, self.far, template)

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
        acoustic.verify('double', self.spoken + far, self.spoken * .9 + far * .01, far, self.physical)
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
