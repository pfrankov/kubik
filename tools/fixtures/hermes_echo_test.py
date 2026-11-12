"""The production Python WASM AEC on independently encoded microphone/reference pairs."""
import importlib.util
import math
from pathlib import Path
import struct
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import wasmtime

ROOT = Path(__file__).resolve().parents[2]
ARTIFACT = ROOT / 'openclaw-kubik/src/vendor/speex-aec.wasm'
LATENCY_SAMPLES = 480  # Verified independently with an impulse below.


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


echo = load_module('hermes_echo_contract', ROOT / 'hermes-kubik/echo.py')
codec = load_module('hermes_echo_codec', ROOT / 'hermes-kubik/codec.py')


def pcm(samples):
    return struct.pack('<' + 'h' * len(samples), *samples)


def samples(data):
    return struct.unpack('<' + 'h' * (len(data) // 2), data)


def energy(data):
    return sum(value * value for value in data)


def aligned_output(cleaner, output):
    # Flush the fixed overlap/add tail, then compare the same input interval.
    length = len(output)
    output.extend(samples(cleaner.process(bytes(1920), bytes(1920))))
    return output[LATENCY_SAMPLES:LATENCY_SAMPLES + length]


def original_process(cleaner, mic, reference):
    """Prior 20 ms copy/call ordering, retained only as an independent PCM oracle."""
    memory, store = cleaner._api['memory'], cleaner._store
    rec, far, clean = cleaner._scratch, cleaner._scratch + 960, cleaner._scratch + 1920
    output = bytearray()
    for at in range(0, 1920, 960):
        memory.write(store, mic[at:at + 960], rec)
        memory.write(store, reference[at:at + 960], far)
        cleaner._api['speex_echo_cancellation'](store, cleaner._state, rec, far, clean)
        cleaner._api['speex_preprocess_run'](store, cleaner._preprocess, clean)
        output.extend(memory.read(store, clean, clean + 960))
    return bytes(output)


class AcousticSignal:
    def __init__(self, delay, gain):
        self.delay, self.gain = delay, gain
        self.seed, self.low = 12345, 0
        self.history = [0] * (24000 * 12)
        self.mic_encoder, self.ref_encoder = [0, 0], [0, 0]

    def next(self, frame):
        mic, reference, near = [], [], []
        for i in range(960):
            at = frame * 960 + i
            self.seed = (self.seed * 1664525 + 1013904223) & 0xffffffff
            self.low = .8 * self.low + .2 * ((self.seed / 0xffffffff - .5) * 16000)
            self.history[at] = int(self.low * self.gain)
            delayed = .45 * self.history[at - self.delay] if at >= self.delay else 0
            delayed += .15 * self.history[at - self.delay - 480] if at >= self.delay + 480 else 0
            voice = round(2200 * (.45 + .55 * math.sin(at / 2400) ** 2) *
                          (math.sin(at * 2 * math.pi * 233 / 24000) +
                           .65 * math.sin(at * 2 * math.pi * 349 / 24000) +
                           .3 * math.sin(at * 2 * math.pi * 701 / 24000))) if frame >= 200 else 0
            mic.append(round(delayed + voice))
            reference.append(self.history[at])
            near.append(voice)
        decoded = codec.decode(codec.encode(pcm(mic), self.mic_encoder))
        reference = codec.decode(codec.encode(pcm(reference), self.ref_encoder))
        return decoded, reference, near


class EchoContractTest(unittest.TestCase):
    def cleaner(self):
        cleaner = echo.EchoCleaner(ARTIFACT)
        self.addCleanup(cleaner.close)
        return cleaner

    def acoustic_case(self, delay, gain):
        cleaner = self.cleaner()
        original = self.cleaner()
        signal = AcousticSignal(delay, gain)
        raw, near_voice, output = [], [], []
        elapsed = 0
        for frame in range(300):
            decoded, reference, near = signal.next(frame)
            start = time.perf_counter()
            cleaned_frame = cleaner.process(decoded, reference)
            elapsed += time.perf_counter() - start
            output.extend(samples(cleaned_frame))
            self.assertEqual(cleaned_frame, original_process(original, decoded, reference))
            raw.extend(samples(decoded))
            near_voice.extend(near)
        cleaned = aligned_output(cleaner, output)
        raw_energy = energy(raw[100 * 960:190 * 960])
        clean_energy = energy(cleaned[100 * 960:190 * 960])
        near, clean_near = near_voice[220 * 960:], cleaned[220 * 960:]
        near_energy, clean_near_energy = energy(near), energy(clean_near)
        correlation = sum(a * b for a, b in zip(near, clean_near))
        erle = 10 * math.log10(raw_energy / clean_energy)
        similarity = correlation / math.sqrt(near_energy * clean_near_energy)
        self.assertGreater(erle, 15, 'Echo was not sufficiently removed')
        self.assertGreater(similarity, .9, 'Double-talk lost the user voice')
        self.assertGreater(clean_near_energy / near_energy, .7, 'Double-talk muted the user')
        self.assertLess(elapsed, .6, 'AEC lacks CPU margin for twelve seconds of audio')
        print(f'Hermes AEC: delay={delay} gain={gain} ERLE={erle:.1f} dB '
              f'near={similarity:.3f} CPU={elapsed * 1000:.1f} ms / 12 s')

    def test_decoded_ima_echo_delays_and_double_talk_preserve_near_speech(self):
        for delay, gain in ((60, .5), (600, 1), (1800, 2)):
            with self.subTest(delay=delay, gain=gain):
                self.acoustic_case(delay, gain)

    def test_near_only_speech_remains_audible_across_adaptation(self):
        cleaner = self.cleaner()
        mic_encoder, ref_encoder = [0, 0], [0, 0]
        seed, previous = 73, 0
        raw, output = [], []
        for frame in range(150):
            near = []
            for i in range(960):
                at = frame * 960 + i
                seed = (seed * 1664525 + 1013904223) & 0xffffffff
                previous = .65 * previous + .35 * ((seed / 0xffffffff - .5) * 6000)
                near.append(round((.15 + .85 * math.sin(at / 2400) ** 2) *
                                  (previous + 1500 * math.sin(at * math.pi * 260 / 24000))))
            decoded = codec.decode(codec.encode(pcm(near), mic_encoder))
            reference = codec.decode(codec.encode(bytes(1920), ref_encoder))
            output.extend(samples(cleaner.process(decoded, reference)))
            raw.extend(samples(decoded))
        cleaned = aligned_output(cleaner, output)
        raw_energy, clean_energy = energy(raw), energy(cleaned)
        correlation = sum(a * b for a, b in zip(raw, cleaned))
        self.assertGreater(clean_energy / raw_energy, .7)
        self.assertGreater(correlation / math.sqrt(clean_energy * raw_energy), .9)

    def test_residual_controls_and_exact_twenty_ms_latency(self):
        cleaner = self.cleaner()
        memory, store = cleaner._api['memory'], cleaner._store
        # Actual library getters establish no AGC/VAD/general noise suppression.
        for request, expected in ((1, 1), (3, 0), (5, 0), (9, 0), (19, 0), (21, -40), (23, -15),
                                  (25, cleaner._state)):
            with self.subTest(control=request):
                self.assertEqual(cleaner._api['speex_preprocess_ctl'](
                    store, cleaner._preprocess, request, cleaner._scratch), 0)
                self.assertEqual(struct.unpack('<i', memory.read(
                    store, cleaner._scratch, cleaner._scratch + 4))[0], expected)
        signal, output = [0] * 9600, []
        signal[960] = 10000
        for at in range(0, len(signal), 960):
            output.extend(samples(cleaner.process(pcm(signal[at:at + 960]), bytes(1920))))
        peak = max(range(len(output)), key=lambda index: abs(output[index]))
        self.assertEqual(peak - 960, LATENCY_SAMPLES)
        self.assertEqual(cleaner.warnings, 0)

    def test_channel_validation_fixed_memory_and_idempotent_close(self):
        cleaner = self.cleaner()
        for left, right in ((None, bytes(1920)), (bytes(1920), None),
                            (bytes(960), bytes(1920)), (bytes(1920), bytes(1922))):
            with self.subTest(lengths=(type(left), type(right))):
                with self.assertRaisesRegex(ValueError, 'synchronized'):
                    cleaner.process(left, right)
        memory, store = cleaner._api['memory'], cleaner._store
        self.assertEqual(memory.data_len(store), 1024 * 1024)
        with self.assertRaises(wasmtime.WasmtimeError):
            memory.grow(store, 1)
        self.assertEqual(cleaner.process(bytes(1920), bytes(1920)), bytes(1920))
        cleaner.close()
        cleaner.close()
        self.assertEqual((cleaner._state, cleaner._preprocess, cleaner._scratch), (0, 0, 0))
        self.assertIsNone(cleaner._store)
        with self.assertRaisesRegex(ValueError, 'synchronized'):
            cleaner.process(bytes(1920), bytes(1920))

    def test_unexpected_import_and_missing_artifact_fail_without_alternate_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'speex-aec.wasm'
            with self.assertRaisesRegex(ValueError, 'artifact'):
                echo.EchoCleaner(path)
            path.write_bytes(wasmtime.wat2wasm('(module (import "env" "network" (func)))'))
            with self.assertRaisesRegex(ValueError, 'Unexpected'):
                echo.EchoCleaner(path)

    def test_preprocessor_initialization_failure_releases_every_owned_state(self):
        factory, close = wasmtime.Instance, echo.EchoCleaner.close
        for failure in ('allocation', 'configuration'):
            with self.subTest(failure=failure):
                events, closed = [], []

                def instance(store, module, imports):
                    api = dict(factory(store, module, imports).exports(store))
                    for name in ('speex_preprocess_state_destroy', 'speex_echo_state_destroy', 'free'):
                        original = api[name]
                        def destroy(*args, name=name, original=original):
                            events.append(name)
                            return original(*args)
                        api[name] = destroy
                    if failure == 'allocation':
                        api['speex_preprocess_state_init'] = lambda *_args: 0
                    else:
                        ctl = api['speex_preprocess_ctl']
                        api['speex_preprocess_ctl'] = lambda *args: -1 if args[2] == 20 else ctl(*args)
                    return SimpleNamespace(exports=lambda _store: api)

                def cleanup(cleaner):
                    close(cleaner)
                    closed.append((cleaner._store, cleaner._api))

                with patch.object(echo.wasmtime, 'Instance', instance), patch.object(echo.EchoCleaner, 'close', cleanup):
                    with self.assertRaisesRegex(ValueError, f'Residual echo suppressor {failure} failed'):
                        echo.EchoCleaner(ARTIFACT)
                expected = ['speex_echo_state_destroy', 'free']
                if failure == 'configuration':
                    expected.insert(0, 'speex_preprocess_state_destroy')
                self.assertEqual(events, expected)
                self.assertEqual(closed, [(None, None)])


class ProcessingBudgetTest(unittest.TestCase):
    def test_budget_counts_processing_and_excludes_test_analysis(self):
        process, unpack = echo.EchoCleaner.process, samples
        for cost in (.001, .003):
            with self.subTest(process_seconds=cost):
                print(f"Synthetic budget clock: filter={cost:g}s/frame, analysis=0.01s/call")
                clock = [0.0]
                case = EchoContractTest()
                self.addCleanup(case.doCleanups)

                def filtered(cleaner, mic, reference):
                    clock[0] += cost
                    return process(cleaner, mic, reference)

                def analyzed(data):
                    clock[0] += .01  # Test-only analysis is deliberately much slower than the filter.
                    return unpack(data)

                with patch.object(time, 'perf_counter', lambda: clock[0]), \
                        patch.object(echo.EchoCleaner, 'process', filtered), \
                        patch.dict(globals(), {'samples': analyzed}):
                    if cost == .001:
                        case.acoustic_case(600, 1)  # 300 ms filter, >3 s analysis: must pass.
                    else:
                        with self.assertRaisesRegex(AssertionError, 'AEC lacks CPU margin'):
                            case.acoustic_case(600, 1)  # 900 ms filter: the unchanged 600 ms cap must fail.


if __name__ == '__main__':
    unittest.main()
