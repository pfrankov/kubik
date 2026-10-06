// What a device does with speech, for tests and the install simulator: decode IMA ADPCM, measure the level.
import { ImaEncoder } from '../src/protocol.js';

/** Decodes one IMA frame payload (int16 LE predictor, u8 step index, nibbles low first) to s16le PCM. */
export function imaDecode(frame) {
  const state = new ImaEncoder();
  state.pred = frame.readInt16LE(0);
  state.index = frame[2];
  const samples = (frame.length - 3) * 2;
  const pcm = Buffer.allocUnsafe(samples * 2);
  for (let i = 0; i < samples; i++) {
    state.step((frame[3 + (i >> 1)] >> ((i & 1) * 4)) & 15);
    pcm.writeInt16LE(state.pred, i * 2);
  }
  return pcm;
}

/** Root-mean-square level of s16le PCM in [0, 1]. */
export function pcmRms(pcm) {
  const samples = pcm.length >> 1;
  let sum = 0;
  for (let i = 0; i < samples; i++) { const v = pcm.readInt16LE(i * 2) / 32768; sum += v * v; }
  return samples ? Math.sqrt(sum / samples) : 0;
}
