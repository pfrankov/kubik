import { spawn } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { existsSync } from 'node:fs';
import { mkdir, readFile, rename, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

const RATE = 24000;
const BYTES_PER_MS = (RATE * 2) / 1000;

export const STT_PHRASES = [
  'Привет! Как дела?',
  'Какая завтра погода?',
  'Расскажи шутку',
  'Напомни мне через пять минут выпить воды',
  'Мне сегодня грустно',
  'Что ты умеешь?',
];

// ---- audio helpers --------------------------------------------------------------------------------------
function rms(pcm) {
  const n = Math.floor(pcm.length / 2);
  if (!n) return 0;
  let sum = 0;
  for (let i = 0; i < n; i++) { const v = pcm.readInt16LE(i * 2) / 32768; sum += v * v; }
  return Math.sqrt(sum / n);
}
function wav(pcm) {
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + pcm.length, 4); h.write('WAVE', 8); h.write('fmt ', 12);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22); h.writeUInt32LE(RATE, 24);
  h.writeUInt32LE(RATE * 2, 28); h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([h, pcm]);
}
/** PCM samples from a WAV file (any fmt chunk order) or raw PCM. */
function pcmFromUpload(buffer) {
  if (buffer.length > 12 && buffer.toString('ascii', 0, 4) === 'RIFF' && buffer.toString('ascii', 8, 12) === 'WAVE') {
    let offset = 12;
    while (offset + 8 <= buffer.length) {
      const id = buffer.toString('ascii', offset, offset + 4);
      const size = buffer.readUInt32LE(offset + 4);
      if (id === 'data') return buffer.subarray(offset + 8, Math.min(buffer.length, offset + 8 + size));
      offset += 8 + size + (size % 2);
    }
    return Buffer.alloc(0);
  }
  return buffer;
}
/** Synthetic "speech": syllable-shaped tones, used when `say` is unavailable or in tests. */
function tonePcm(text) {
  const syllables = Math.max(2, Math.min(60, Math.round(String(text).length / 3)));
  const sylMs = 140;
  const pcm = Buffer.alloc(Math.round((syllables * sylMs + 200) * BYTES_PER_MS));
  for (let s = 0; s < syllables; s++) {
    const freq = 180 + ((s * 37) % 120);
    const start = Math.round(s * sylMs * RATE / 1000);
    const len = Math.round((sylMs - 30) * RATE / 1000);
    for (let i = 0; i < len; i++) {
      const env = Math.sin(Math.PI * i / len);
      const v = Math.round(9000 * env * Math.sin(2 * Math.PI * freq * i / RATE));
      pcm.writeInt16LE(v, (start + i) * 2);
    }
  }
  return pcm;
}
function run(cmd, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, { stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    child.stderr.on('data', (d) => { err += d; });
    child.on('error', reject);
    child.on('exit', (code) => (code === 0 ? resolve() : reject(new Error(`${cmd} exited ${code}: ${err.slice(0, 200)}`))));
  });
}

export function createSynth({ mode = 'say', cacheDir = join(HERE, '.cache'), voice = 'Milena', ffmpeg = existsSync('/opt/homebrew/bin/ffmpeg') ? '/opt/homebrew/bin/ffmpeg' : 'ffmpeg', log = () => {} } = {}) {
  const inflight = new Map();
  let sayBroken = mode !== 'say';
  return async function synthesize(text) {
    const clean = String(text).trim() || '...';
    if (sayBroken) return tonePcm(clean);
    const key = createHash('sha256').update(`${voice}\n${clean}`).digest('hex');
    const file = join(cacheDir, `${key}.pcm`);
    if (existsSync(file)) return readFile(file);
    if (inflight.has(key)) return inflight.get(key);
    const job = (async () => {
      await mkdir(cacheDir, { recursive: true });
      const tmp = join(tmpdir(), `kubik-mock-${randomUUID()}`);
      try {
        await run('say', ['-v', voice, '-o', `${tmp}.aiff`, '--', clean]);
        await run(ffmpeg, ['-y', '-loglevel', 'error', '-i', `${tmp}.aiff`, '-f', 's16le', '-acodec', 'pcm_s16le', '-ar', String(RATE), '-ac', '1', `${tmp}.pcm`]);
        const pcm = await readFile(`${tmp}.pcm`);
        await writeFile(`${file}.tmp`, pcm);
        await rename(`${file}.tmp`, file);
        return pcm;
      } catch (error) {
        log(`mock tts: say/ffmpeg failed (${error.message}); falling back to tones`);
        sayBroken = true;
        return tonePcm(clean);
      } finally {
        await rm(`${tmp}.aiff`, { force: true }); await rm(`${tmp}.pcm`, { force: true });
        inflight.delete(key);
      }
    })();
    inflight.set(key, job);
    return job;
  };
}

export function createStt({ minMs = 250, minRms = 0.01, phrases = STT_PHRASES, start = 0 } = {}) {
  let next = start;
  return (pcm) => {
    if (pcm.length / BYTES_PER_MS < minMs || rms(pcm) < minRms) return '';
    return phrases[next++ % phrases.length];
  };
}

export { RATE, BYTES_PER_MS, rms, wav, pcmFromUpload };

