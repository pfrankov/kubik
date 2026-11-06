"""Per-conversation SpeexDSP AEC/residual suppression; fixed memory, 20 ms delay."""
from functools import lru_cache
from pathlib import Path
import struct

import wasmtime

FRAME_BYTES = 960  # 20 ms, mono PCM16 at 24 kHz.
MEMORY_BYTES = 1024 * 1024


@lru_cache(maxsize=1)
def compiled(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 128 * 1024:
        raise ValueError('Invalid echo canceller artifact')
    engine = wasmtime.Engine()
    return engine, wasmtime.Module(engine, path.read_bytes())


class EchoCleaner:
    def __init__(self, wasm_path=None):
        # The installer places the canonical artifact here. Source tests pass its
        # repository path explicitly; a missing installed artifact is an error.
        path = Path(wasm_path) if wasm_path is not None else Path(__file__).with_name('speex-aec.wasm')
        engine, module = compiled(str(path))
        self._store = wasmtime.Store(engine)
        self._store.set_limits(memory_size=MEMORY_BYTES, instances=1, memories=1, tables=1)
        self._api = None
        self._state = self._scratch = self._preprocess = 0
        self.warnings = 0
        try:
            imports = self._imports(module)
            self._api = wasmtime.Instance(self._store, module, imports).exports(self._store)
            self._api['_initialize'](self._store)
            self._state = self._api['speex_echo_state_init'](self._store, 480, 2400)
            self._scratch = self._api['malloc'](self._store, FRAME_BYTES * 3)
            if not self._state or not self._scratch:
                raise ValueError('Echo canceller allocation failed')
            self._api['memory'].write(self._store, struct.pack('<i', 24000), self._scratch)
            if self._api['speex_echo_ctl'](self._store, self._state, 24, self._scratch):
                raise ValueError('Echo canceller sample rate failed')
            self._configure_preprocessor()
        except BaseException:
            self.close()
            raise

    def _configure_preprocessor(self):
        self._preprocess = self._api['speex_preprocess_state_init'](self._store, 480, 24000)
        if not self._preprocess:
            raise ValueError('Residual echo suppressor allocation failed')
        ctl = self._api['speex_preprocess_ctl']
        # SET_ECHO_STATE takes the state itself, unlike the integer controls.
        if ctl(self._store, self._preprocess, 24, self._state):
            raise ValueError('Residual echo suppressor attachment failed')
        # Keep the spectral processor enabled, but attenuate only residual echo.
        # DENOISE=0 would bypass echo suppression as well. VAD defaults to off.
        for request, value in ((0, 1), (2, 0), (8, 0), (18, 0), (20, -40), (22, -15)):
            self._api['memory'].write(self._store, struct.pack('<i', value), self._scratch)
            if ctl(self._store, self._preprocess, request, self._scratch):
                raise ValueError('Residual echo suppressor configuration failed')

    def _imports(self, module):
        callbacks = {'fd_close': lambda *_args: 8, 'fd_seek': lambda *_args: 70,
                     'fd_write': self._write_diagnostic}
        result = []
        for imported in module.imports:
            if imported.module != 'wasi_snapshot_preview1' or imported.name not in callbacks:
                raise ValueError('Unexpected echo canceller import')
            result.append(wasmtime.Func(self._store, imported.type, callbacks[imported.name],
                                       access_caller=imported.name == 'fd_write'))
        return result

    def _write_diagnostic(self, caller, fd, vectors, count, written):
        # Speex diagnostics contain no audio. Count them without exposing their
        # contents or granting the WASM instance any file/network access.
        if fd not in (1, 2):
            return 8
        memory = caller.get('memory')
        descriptors = memory.read(caller, vectors, vectors + count * 8)
        size = sum(struct.unpack_from('<I', descriptors, at + 4)[0]
                   for at in range(0, len(descriptors), 8))
        memory.write(caller, struct.pack('<I', size), written)
        self.warnings += 1
        return 0

    def process(self, pcm, reference):
        if (not self._state or not isinstance(pcm, bytes) or not isinstance(reference, bytes)
                or len(pcm) != 1920 or len(reference) != 1920):
            raise ValueError('Live requires synchronized 40 ms mic/reference')
        memory, store = self._api['memory'], self._store
        rec, far, clean = self._scratch, self._scratch + FRAME_BYTES, self._scratch + FRAME_BYTES * 2
        output = bytearray()
        for at in range(0, len(pcm), FRAME_BYTES):
            memory.write(store, pcm[at:at + FRAME_BYTES], rec)
            memory.write(store, reference[at:at + FRAME_BYTES], far)
            self._api['speex_echo_cancellation'](store, self._state, rec, far, clean)
            self._api['speex_preprocess_run'](store, self._preprocess, clean)
            output.extend(memory.read(store, clean, clean + FRAME_BYTES))
        return bytes(output)

    def close(self):
        if self._store is None:
            return
        try:
            if self._preprocess:
                self._api['speex_preprocess_state_destroy'](self._store, self._preprocess)
            if self._state:
                self._api['speex_echo_state_destroy'](self._store, self._state)
            if self._scratch:
                self._api['free'](self._store, self._scratch)
        finally:
            self._state = self._scratch = self._preprocess = 0
            self._store.close()
            self._store = self._api = None
