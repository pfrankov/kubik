"""Verify known speech through a measured acoustic path before/after AEC."""
from pathlib import Path
import sys
import wave
import numpy as np

RATE = 24000
MAX_CAPTURE_SAMPLES = 30 * RATE
CLEAN_DELAY_SAMPLES = 480  # Speex Preprocessor overlap-add: one 20 ms frame.
BANDS = ((100, 300), (300, 600), (600, 1000), (1000, 1600),
         (1600, 2500), (2500, 4000), (4000, 6000))


def envelope(signal, size=480, hop=480):
    frames = np.lib.stride_tricks.sliding_window_view(signal, size)[::hop]
    return np.sqrt(np.mean(frames ** 2, axis=1))


def band_envelopes(signal):
    frames = np.lib.stride_tricks.sliding_window_view(signal, 1200)[::480]
    powers = abs(np.fft.rfft(frames * np.hanning(1200), axis=1)) ** 2
    bins = np.fft.rfftfreq(1200, 1 / RATE)
    return np.array([np.sqrt(np.sum(powers[:, (bins >= lo) & (bins < hi)], axis=1))
                     for lo, hi in BANDS]).T


def capture_bound(signal):
    assert len(signal) <= MAX_CAPTURE_SAMPLES, 'Acoustic capture exceeds the 30 second bound'


def read_capture(path):
    size = path.stat().st_size
    assert size <= MAX_CAPTURE_SAMPLES * 2, 'Acoustic capture exceeds the 30 second bound'
    assert size % 2 == 0, 'Acoustic capture contains incomplete PCM16 samples'
    signal = np.fromfile(path, '<i2', count=MAX_CAPTURE_SAMPLES + 1).astype(float)
    capture_bound(signal)
    return signal


def calibrate(raw, source):
    """Identify the phrase without comparing phase across separate audio clocks."""
    capture_bound(raw)
    capture_bound(source)
    known, recorded = envelope(source), envelope(raw)
    count = len(recorded) - len(known) + 1
    assert count > 0, 'Missing complete calibration phrase'
    scores = [np.corrcoef(recorded[i:i + len(known)], known)[0, 1] for i in range(count)]
    at = int(np.argmax(scores))
    lag = at * 480
    template = raw[lag:lag + len(source)]
    a, b = band_envelopes(template), band_envelopes(source)
    bands = np.median([np.corrcoef(a[:, i], b[:, i])[0, 1] for i in range(len(BANDS))])
    print(f'CALIBRATION: envelope={scores[at]:.3f}, bands={bands:.3f}, lag={lag / 24:.1f}ms')
    assert scores[at] > .8 and bands > .65, (
        f'Known external phrase is absent from calibration: envelope={scores[at]:.3f} (required >0.800), '
        f'bands={bands:.3f} (required >0.650), lag={lag / 24:.1f}ms')
    return template, lag


def projection(signal, template, lag=None):
    """Match a physical microphone template; clean uses raw's lag plus fixed DSP delay."""
    capture_bound(signal)
    capture_bound(template)
    if lag is None:
        count = len(signal) - len(template) + 1
        assert count > 0, 'Missing complete physical phrase'
        n = 1 << (len(signal) + len(template) - 1).bit_length()
        correlation = np.fft.irfft(np.fft.rfft(signal, n) * np.fft.rfft(template[::-1], n), n)
        start = len(template) - 1
        lag = int(np.argmax(np.abs(correlation[start:start + count])))
    a = signal[lag:lag + len(template)]
    assert len(a) == len(template), 'Truncated matched phrase'
    energy = np.dot(template, template)
    assert energy > 0 and np.dot(a, a) > 0, 'Silent matched phrase'
    return float(np.dot(a, template) / energy), float(np.dot(a, template) / np.sqrt(np.dot(a, a) * energy)), lag


def verify(label, raw, clean, reference, template, lag=None):
    assert len(raw) == len(clean) == len(reference), 'Unmatched capture lengths'
    before, corr_raw, lag = projection(raw, template, lag)
    after, corr_clean, _ = projection(clean, template, lag + CLEAN_DELAY_SAMPLES)
    overlap = reference[lag:lag + 3 * RATE]
    assert len(overlap) == 3 * RATE, 'Missing matched reference interval'
    ref_rms = np.sqrt(np.mean(overlap ** 2))
    assert ref_rms < 100 if label == 'near' else ref_rms > 100, 'Unexpected speaker overlap'
    assert abs(corr_raw) > .12, (
        f'Raw near-speech match is below the confidence threshold: correlation={corr_raw:.3f} '
        f'(required absolute >0.120), lag={lag / 24:.1f}ms')
    ratio = abs(after / before)
    print(f'NEAR physical {label}: raw correlation={corr_raw:.3f}, clean={corr_clean:.3f}, speech gain={ratio:.3f}')
    assert ratio > .65, 'Echo cancellation suppressed the known near-end speech'
    assert abs(corr_clean) > .12, 'Known speech is absent after cancellation'


def main():
    with wave.open(sys.argv[1]) as source:
        assert (source.getnchannels(), source.getsampwidth(), source.getframerate()) == (1, 2, RATE)
        assert 5 * RATE <= source.getnframes() <= 6 * RATE, 'Use a 5–6 second near-end WAV'
        known = np.frombuffer(source.readframes(source.getnframes()), '<i2').astype(float)
    assert 5 * RATE <= len(known) <= 6 * RATE, 'Use a 5–6 second near-end WAV'
    captures = {label: [read_capture(Path(sys.argv[2]) / f'{label}-{kind}.pcm')
                       for kind in ('mic', 'clean', 'reference')] for label in ('near', 'double')}
    template, lag = calibrate(captures['near'][0], known)
    verify('near', *captures['near'], template, lag)
    verify('double', *captures['double'], template)


if __name__ == '__main__': main()
