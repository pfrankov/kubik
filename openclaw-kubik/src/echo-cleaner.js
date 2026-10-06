import { readFileSync } from 'node:fs';

// Pinned Xiph SpeexDSP 1.2.1 MDF, built by tools/build-echo-wasm.py.
// One private 1 MiB WASM memory per Live conversation; no native addon or subprocess.
// OpenClaw's plugin loader imports synchronous ESM through require().
const module = new WebAssembly.Module(readFileSync(new URL('./vendor/speex-aec.wasm', import.meta.url)));
const FRAME_SAMPLES = 480; // 20 ms at 24 kHz
const FRAME_BYTES = FRAME_SAMPLES * 2;

export class EchoCleaner {
  #api;
  #state;
  #scratch;
  warnings = 0;
  constructor() {
    const imports = { wasi_snapshot_preview1: {
      fd_close: () => 8, fd_seek: () => 70,
      fd_write: (fd, vectors, count, written) => {
        // Speex diagnostics contain no audio; count them without logging every clipped frame.
        if (fd !== 1 && fd !== 2) return 8;
        const view = new DataView(this.#api.memory.buffer);
        let bytes = 0;
        for (let i = 0; i < count; i++) bytes += view.getUint32(vectors + i * 8 + 4, true);
        view.setUint32(written, bytes, true); this.warnings++; return 0;
      } } };
    this.#api = new WebAssembly.Instance(module, imports).exports;
    this.#api._initialize();
    this.#state = this.#api.speex_echo_state_init(FRAME_SAMPLES, 2400); // 100 ms acoustic tail
    this.#scratch = this.#api.malloc(FRAME_BYTES * 3);
    if (!this.#state || !this.#scratch) { this.close(); throw Error('Echo canceller allocation failed'); }
    new DataView(this.#api.memory.buffer).setInt32(this.#scratch, 24000, true);
    if (this.#api.speex_echo_ctl(this.#state, 24, this.#scratch) !== 0) { // SPEEX_ECHO_SET_SAMPLING_RATE
      this.close(); throw Error('Echo canceller sample rate failed');
    }
  }
  process(pcm, reference) {
    if (!this.#state || !Buffer.isBuffer(pcm) || !Buffer.isBuffer(reference) ||
        pcm.length !== 1920 || reference.length !== pcm.length) throw Error('Live requires synchronized 40 ms mic/reference');
    const memory = new Uint8Array(this.#api.memory.buffer);
    const rec = this.#scratch, far = rec + FRAME_BYTES, clean = far + FRAME_BYTES;
    const output = Buffer.allocUnsafe(pcm.length);
    for (let at = 0; at < pcm.length; at += FRAME_BYTES) {
      memory.set(pcm.subarray(at, at + FRAME_BYTES), rec);
      memory.set(reference.subarray(at, at + FRAME_BYTES), far);
      this.#api.speex_echo_cancellation(this.#state, rec, far, clean);
      output.set(memory.subarray(clean, clean + FRAME_BYTES), at);
    }
    return output;
  }
  close() {
    if (this.#state) this.#api.speex_echo_state_destroy(this.#state);
    if (this.#scratch) this.#api.free(this.#scratch);
    this.#state = this.#scratch = 0;
  }
}
