#!/usr/bin/env node
// Objective checks for the rendered kit (run after generate.mjs):
//   loudness + true peak (ffmpeg ebur128), DC (astats), energy below 200 Hz,
//   first/last samples, total size; plus out/sheet.png spectrogram contact sheet.
//
// Clips shorter than one 400 ms EBU block read as -70 LUFS in ebur128, so they
// are measured looped (-stream_loop), which gives the clip's own mean loudness.

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const PCM_DIR = path.join(ROOT, 'firmware/assets/sfx');
const OUT = path.join(HERE, 'out');
const FFMPEG = process.env.FFMPEG || '/opt/homebrew/bin/ffmpeg';

const ff = (args) => {
  try {
    return execFileSync(FFMPEG, ['-hide_banner', '-nostats', ...args], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (e) {
    return (e.stdout || '') + (e.stderr || '');
  }
};
const ffErr = (args) => {
  const r = execFileSync('sh', ['-c', `"${FFMPEG}" -hide_banner -nostats ${args} 2>&1`], { encoding: 'utf8' });
  return r;
};
const num = (re, s) => {
  const m = re.exec(s);
  return m ? parseFloat(m[1]) : NaN;
};

const manifest = JSON.parse(fs.readFileSync(path.join(PCM_DIR, 'manifest.json'), 'utf8'));
const rows = [];
let total = 0;
for (const e of manifest) {
  const wav = path.join(OUT, `${e.name}.wav`);
  const pcmBuf = fs.readFileSync(path.join(PCM_DIR, e.file));
  total += pcmBuf.length;
  const pcm = new Int16Array(pcmBuf.buffer, pcmBuf.byteOffset, pcmBuf.length / 2);
  const loops = e.ms < 400 ? Math.ceil(1600 / e.ms) : 0;
  const eb = ffErr(`${loops ? `-stream_loop ${loops}` : ''} -i "${wav}" -af ebur128=peak=true:framelog=quiet -f null -`);
  const I = num(/I:\s+(-?[\d.]+) LUFS/, eb);
  const TP = num(/Peak:\s+(-?[\d.]+) dBFS/, eb);
  const as = ffErr(`-i "${wav}" -af astats=metadata=0 -f null -`);
  const dc = num(/Overall[\s\S]*?DC offset: (-?[\d.e-]+)/, as);
  const rms = num(/Overall[\s\S]*?RMS level dB: (-?[\d.inf]+)/, as);
  const lo = ffErr(`-i "${wav}" -af lowpass=f=200,lowpass=f=200,lowpass=f=200,lowpass=f=200,astats=metadata=0 -f null -`);
  const rmsLo = /RMS level dB: -inf/.test(lo.slice(lo.indexOf('Overall'))) ? -200 : num(/Overall[\s\S]*?RMS level dB: (-?[\d.]+)/, lo);
  const edge = 6; // 0.25 ms
  let maxHead = 0, maxTail = 0;
  for (let i = 0; i < edge; i++) {
    maxHead = Math.max(maxHead, Math.abs(pcm[i]));
    maxTail = Math.max(maxTail, Math.abs(pcm[pcm.length - 1 - i]));
  }
  rows.push({ name: e.name, ms: e.ms, I, TP, dc, low: rmsLo - rms, first: pcm[0], last: pcm[pcm.length - 1], maxHead, maxTail, looped: !!loops });
}

const pad = (s, n) => String(s).padStart(n);
console.log('name              ms   LUFS(ffmpeg)  dBTP   DC        <200Hz   first last  max|x| first/last 0.25ms');
for (const r of rows)
  console.log(
    `${r.name.padEnd(16)}${pad(r.ms, 5)}  ${pad(r.I.toFixed(1), 6)}${r.looped ? ' (loop)' : '       '}${pad(r.TP.toFixed(1), 6)}  ${pad(r.dc.toExponential(1), 8)}  ${r.low < -150 ? '  -inf' : pad(r.low.toFixed(1), 6)} dB ${pad(r.first, 4)} ${pad(r.last, 4)}  ${pad(r.maxHead, 4)} ${pad(r.maxTail, 4)}`,
  );
const bad = rows.filter((r) => r.TP > -3 || r.first !== 0 || r.last !== 0 || r.low > -20);
console.log(`\ntotal PCM: ${total} bytes (${(total / 1024).toFixed(1)} KiB, ${(total / 48000).toFixed(2)} s)`);
console.log(bad.length ? `FAIL: ${bad.map((r) => r.name).join(', ')}` : 'all checks passed (TP <= -3 dBTP, edges = 0, <200 Hz at least 20 dB down)');

// ------------------------------------------------------------ contact sheet
const TW = 300, SH = 150, LH = 16, COLS = 6;
const tmp = path.join(OUT, '.sheet');
fs.rmSync(tmp, { recursive: true, force: true });
fs.mkdirSync(tmp, { recursive: true });

// 3x5 pixel font, drawn at 2x.
const FONT = {
  A: '010101111101101', B: '110101110101110', C: '011100100100011', D: '110101101101110', E: '111100110100111',
  F: '111100110100100', G: '011100101101011', H: '101101111101101', I: '111010010010111', J: '001001001101010',
  K: '101101110101101', L: '100100100100111', M: '101111111101101', N: '110101101101101', O: '010101101101010',
  P: '110101110100100', Q: '010101101110011', R: '110101110101101', S: '011100010001110', T: '111010010010010',
  U: '101101101101111', V: '101101101101010', W: '101101111111101', X: '101101010101101', Y: '101101010010010',
  Z: '111001010100111', 0: '111101101101111', 1: '010110010010111', 2: '110001010100111', 3: '110001010001110',
  4: '101101111001001', 5: '111100110001110', 6: '011100111101111', 7: '111001010010010', 8: '111101111101111',
  9: '111101111001110', _: '000000000000111', ' ': '000000000000000', '.': '000000000000010', '-': '000000111000000',
};
function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function png(w, h, rgb) {
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
function label(text) {
  const img = Buffer.alloc(TW * LH * 3, 24);
  let x = 4;
  for (const ch of text.toUpperCase()) {
    const g = FONT[ch] ?? FONT[' '];
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 3; c++)
        if (g[r * 3 + c] === '1')
          for (let dy = 0; dy < 2; dy++)
            for (let dx = 0; dx < 2; dx++) {
              const px = x + c * 2 + dx, py = 3 + r * 2 + dy;
              if (px < TW) img.fill(230, (py * TW + px) * 3, (py * TW + px) * 3 + 3);
            }
    x += 8;
  }
  return png(TW, LH, img);
}

const n = Math.ceil(rows.length / COLS) * COLS;
for (let i = 0; i < n; i++) {
  const out = path.join(tmp, `${String(i).padStart(3, '0')}.png`);
  if (i >= rows.length) {
    ff(['-f', 'lavfi', '-i', `color=c=0x181818:s=${TW}x${SH + LH}`, '-frames:v', '1', '-y', out]);
    continue;
  }
  const r = rows[i];
  const lab = path.join(tmp, `lab${i}.png`);
  fs.writeFileSync(lab, label(`${r.name} ${r.ms}MS ${r.I.toFixed(0)}`));
  ff([
    '-i', path.join(OUT, `${r.name}.wav`), '-i', lab,
    '-filter_complex', `[0:a]showspectrumpic=s=${TW}x${SH}:legend=0:start=0:stop=8000:drange=70:color=magma:scale=log[s];[1:v][s]vstack`,
    '-frames:v', '1', '-y', out,
  ]);
}
ff(['-framerate', '1', '-i', path.join(tmp, '%03d.png'), '-vf', `tile=${COLS}x${n / COLS}:padding=4:color=0x000000`, '-frames:v', '1', '-y', path.join(OUT, 'sheet.png')]);
fs.rmSync(tmp, { recursive: true, force: true });
console.log(`sheet: ${path.join(OUT, 'sheet.png')} (each tile 0..8 kHz, full clip length; order = manifest)`);
