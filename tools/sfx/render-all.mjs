import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { SOUNDS } from './sounds.mjs';
import { FS_OUT } from './synth.mjs';
import { render, wav } from './dsp.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const PCM_DIR = path.join(ROOT, 'firmware/assets/sfx');
const WAV_DIR = path.join(HERE, 'out');

function removeStale(dir, extension, names) {
  for (const file of fs.readdirSync(dir)) {
    const name = file.slice(0, -extension.length);
    if (file.endsWith(extension) && !names.has(name)) fs.unlinkSync(path.join(dir, file));
  }
}

function gainNotes(sound, result) {
  return [
    `target ${result.target} LUFS`,
    result.gr > 0.05 ? `transient limiter ${result.gr.toFixed(1)} dB` : null,
    result.lufs < result.target - 0.3 ? `peak-bound at ${result.lufs.toFixed(1)} LUFS` : null,
    sound.group === 'think' ? 'kept ~8 dB under the kit' : null,
  ].filter(Boolean).join('; ');
}

function manifestEntry(sound, result, counts, notes) {
  return {
    name: sound.name,
    file: `${sound.name}.pcm`,
    ms: Math.round((result.pcm.length / FS_OUT) * 1000),
    group: sound.group ?? sound.name,
    variant: sound.group ? +sound.name.split('_').pop() : 1,
    variants: sound.group ? counts[sound.group] : 1,
    lufs: +result.lufs.toFixed(1),
    true_peak_dbtp: +result.tp.toFixed(1),
    gain_notes: notes,
    design: sound.idea,
  };
}

function renderSound(sound, counts) {
  if (sound.max && sound.ms > sound.max) throw new Error(`${sound.name}: ${sound.ms} ms exceeds ${sound.max} ms`);
  const result = render(sound);
  const bytes = Buffer.from(result.pcm.buffer);
  fs.writeFileSync(path.join(PCM_DIR, `${sound.name}.pcm`), bytes);
  fs.writeFileSync(path.join(WAV_DIR, `${sound.name}.wav`), wav(result.pcm));
  const entry = manifestEntry(sound, result, counts, gainNotes(sound, result));
  console.log(`${sound.name.padEnd(16)} ${String(entry.ms).padStart(5)} ms  ${entry.lufs.toFixed(1).padStart(6)} LUFS  ${entry.true_peak_dbtp.toFixed(1).padStart(5)} dBTP  ${result.gr > 0.05 ? `GR ${result.gr.toFixed(1)} dB` : ''}`);
  return { entry, byteLength: bytes.length };
}

export function main(args = process.argv.slice(2)) {
  const selected = args.length ? SOUNDS.filter((sound) => args.some((prefix) => sound.name.startsWith(prefix))) : SOUNDS;
  fs.mkdirSync(PCM_DIR, { recursive: true });
  fs.mkdirSync(WAV_DIR, { recursive: true });
  if (!args.length) {
    const keep = new Set(SOUNDS.map((sound) => sound.name));
    removeStale(PCM_DIR, '.pcm', keep);
    removeStale(WAV_DIR, '.wav', keep);
  }
  const groupSize = {};
  for (const sound of SOUNDS) if (sound.group) groupSize[sound.group] = (groupSize[sound.group] || 0) + 1;
  const manifestPath = path.join(PCM_DIR, 'manifest.json');
  const old = args.length && fs.existsSync(manifestPath) ? JSON.parse(fs.readFileSync(manifestPath, 'utf8')) : [];
  const byName = new Map(old.map((entry) => [entry.name, entry]));
  let total = 0;
  for (const sound of selected) {
    const result = renderSound(sound, groupSize);
    byName.set(result.entry.name, result.entry);
    total += result.byteLength;
  }
  const order = SOUNDS.map((sound) => sound.name);
  const manifest = [...byName.values()].filter((entry) => order.includes(entry.name))
    .sort((a, b) => order.indexOf(a.name) - order.indexOf(b.name));
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + '\n');
  const all = manifest.reduce((sum, entry) => sum + fs.statSync(path.join(PCM_DIR, entry.file)).size, 0);
  console.log(`\n${manifest.length} sounds, ${(all / 1024).toFixed(1)} KiB PCM total (${selected.length} rendered, ${(total / 1024).toFixed(1)} KiB)`);
}
