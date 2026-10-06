import {
  FS, FS_OUT, TAU, TARGET_LUFS, CEIL_DBTP, MAX_LIMIT_DB, HPF_HZ, LPF_HZ,
  Track, hashStr, rc, Biquad,
} from './synth.mjs';

// Small Schroeder reverb: 4 damped combs in parallel + 2 allpasses.
function reverb(x, { rt = 0.5, wet = 0.14, pre = 0.008, damp = 0.35 } = {}) {
  const combsMs = [23.3, 26.9, 30.7, 33.9];
  const apMs = [5.0, 1.7];
  const n = x.length;
  const preN = Math.round(pre * FS);
  const wetSig = new Float64Array(n);
  for (const ms of combsMs) {
    const d = Math.round((ms * FS) / 1000);
    const g = 10 ** ((-3 * (ms / 1000)) / rt);
    const buf = new Float64Array(d);
    let lp = 0, p = 0;
    for (let i = 0; i < n; i++) {
      const inp = i >= preN ? x[i - preN] : 0;
      const out = buf[p];
      lp = out * (1 - damp) + lp * damp;
      buf[p] = inp * 0.25 + lp * g;
      wetSig[i] += out;
      p = (p + 1) % d;
    }
  }
  for (const ms of apMs) {
    const d = Math.round((ms * FS) / 1000);
    const buf = new Float64Array(d);
    let p = 0;
    const g = 0.5;
    for (let i = 0; i < n; i++) {
      const bo = buf[p];
      const v = wetSig[i] + g * bo;
      buf[p] = v;
      wetSig[i] = bo - g * v;
      p = (p + 1) % d;
    }
  }
  // Match wet RMS to dry RMS so `wet` really is a proportion.
  let sd = 0, sw = 0;
  for (let i = 0; i < n; i++) { sd += x[i] * x[i]; sw += wetSig[i] * wetSig[i]; }
  const k = sw > 0 ? Math.sqrt(sd / sw) : 0;
  const y = new Float64Array(n);
  for (let i = 0; i < n; i++) y[i] = (1 - wet) * x[i] + wet * k * wetSig[i];
  return y;
}

// Windowed-sinc FIR, 48k -> 24k, zero delay (symmetric, centred).
function decimate2(x) {
  const taps = 127, c = (taps - 1) / 2, fc = 10000 / FS;
  const h = new Float64Array(taps);
  let sum = 0;
  for (let k = 0; k < taps; k++) {
    const m = k - c;
    const sinc = m === 0 ? 2 * fc : Math.sin(TAU * fc * m) / (Math.PI * m);
    const w = 0.42 - 0.5 * Math.cos((TAU * k) / (taps - 1)) + 0.08 * Math.cos((2 * TAU * k) / (taps - 1));
    h[k] = sinc * w;
    sum += h[k];
  }
  for (let k = 0; k < taps; k++) h[k] /= sum;
  const n = Math.floor(x.length / 2);
  const y = new Float64Array(n);
  for (let m = 0; m < n; m++) {
    let s = 0;
    const ctr = 2 * m;
    for (let k = 0; k < taps; k++) {
      const j = ctr + c - k;
      if (j >= 0 && j < x.length) s += h[k] * x[j];
    }
    y[m] = s;
  }
  return y;
}

// ------------------------------------------------------------------ measurement
// ITU-R BS.1770-4 K-weighting + gating. For clips shorter than one 400 ms
// block the whole clip is a single block (otherwise the measure is undefined).
// Filter design as in libebur128 / ffmpeg's ebur128, valid at any sample rate.
function kWeight(x, fs) {
  const y = Float64Array.from(x);
  let K = Math.tan((Math.PI * 1681.974450955533) / fs), Q = 0.7071752369554196;
  const Vh = 10 ** (3.999843853973347 / 20), Vb = Vh ** 0.4996667741545416;
  let a0 = 1 + K / Q + K * K;
  new Biquad().set(Vh + (Vb * K) / Q + K * K, 2 * (K * K - Vh), Vh - (Vb * K) / Q + K * K, a0, 2 * (K * K - 1), 1 - K / Q + K * K).run(y);
  K = Math.tan((Math.PI * 38.13547087602444) / fs); Q = 0.5003270373238773;
  a0 = 1 + K / Q + K * K;
  new Biquad().set(a0, -2 * a0, a0, a0, 2 * (K * K - 1), 1 - K / Q + K * K).run(y);
  return y;
}
function loudness(x, fs = FS_OUT) {
  const y = kWeight(x, fs);
  const blk = Math.round(0.4 * fs), hop = Math.round(0.1 * fs);
  const ms = (a, b) => { let s = 0; for (let i = a; i < b; i++) s += y[i] * y[i]; return s / (b - a); };
  const L = (z) => -0.691 + 10 * Math.log10(z + 1e-30);
  if (y.length < blk) return L(ms(0, y.length));
  const zs = [];
  for (let a = 0; a + blk <= y.length; a += hop) zs.push(ms(a, a + blk));
  const g1 = zs.filter((z) => L(z) > -70);
  if (!g1.length) return -70;
  const rel = L(g1.reduce((s, z) => s + z, 0) / g1.length) - 10;
  const g2 = g1.filter((z) => L(z) > rel);
  return L(g2.reduce((s, z) => s + z, 0) / g2.length);
}
// 4x oversampled peak estimate (true peak).
const TP_H = (() => {
  const half = 12, taps = [];
  for (let p = 0; p < 4; p++) {
    const row = [];
    for (let k = -half; k <= half; k++) {
      const t = k - p / 4;
      const s = t === 0 ? 1 : Math.sin(Math.PI * t) / (Math.PI * t);
      const w = 0.5 + 0.5 * Math.cos((Math.PI * t) / (half + 1));
      row.push(s * w);
    }
    taps.push(row);
  }
  return { half, taps };
})();
function truePeakEnv(x) {
  const { half, taps } = TP_H;
  const env = new Float64Array(x.length);
  for (let n = 0; n < x.length; n++) {
    let m = Math.abs(x[n]);
    for (let p = 1; p < 4; p++) {
      let s = 0;
      const row = taps[p];
      for (let k = -half; k <= half; k++) {
        const j = n + k;
        if (j >= 0 && j < x.length) s += row[k + half] * x[j];
      }
      m = Math.max(m, Math.abs(s));
    }
    env[n] = m;
  }
  return env;
}
const truePeak = (x) => truePeakEnv(x).reduce((a, b) => Math.max(a, b), 0);
const db = (v) => 20 * Math.log10(v + 1e-30);
const lin = (d) => 10 ** (d / 20);

// Transparent-ish peak limiter: min-filter the required gain over +-h, then
// Hann-smooth over the same span, so the gain never exceeds what is required.
function limit(x, ceil) {
  const env = truePeakEnv(x);
  const h = Math.round(0.004 * FS_OUT);
  const req = env.map((v) => (v > ceil ? ceil / v : 1));
  const mn = new Float64Array(x.length);
  for (let i = 0; i < x.length; i++) {
    let m = 1;
    for (let k = Math.max(0, i - h); k <= Math.min(x.length - 1, i + h); k++) if (req[k] < m) m = req[k];
    mn[i] = m;
  }
  const w = [];
  let ws = 0;
  for (let k = -h; k <= h; k++) { const v = 0.5 + 0.5 * Math.cos((Math.PI * k) / (h + 1)); w.push(v); ws += v; }
  const y = new Float64Array(x.length);
  let minG = 1;
  for (let i = 0; i < x.length; i++) {
    let s = 0;
    for (let k = -h; k <= h; k++) {
      const j = Math.min(x.length - 1, Math.max(0, i + k));
      s += w[k + h] * mn[j];
    }
    const g = s / ws;
    minG = Math.min(minG, g);
    y[i] = x[i] * g;
  }
  return { y, grDb: -db(minG) };
}

function normalise(x, target) {
  const ceil = lin(CEIL_DBTP) * 0.995;
  let g = lin(target - loudness(x));
  let y = x, gr = 0, peakBound = false;
  for (let it = 0; it < 6; it++) {
    const s = x.map((v) => v * g);
    const tp = truePeak(s);
    if (tp > ceil) {
      if (tp / ceil > lin(MAX_LIMIT_DB)) {
        g *= (ceil * lin(MAX_LIMIT_DB)) / tp; // limiter would work too hard: turn down
        peakBound = true;
      }
      ({ y, grDb: gr } = limit(x.map((v) => v * g), ceil));
    } else {
      y = s;
      gr = 0;
    }
    const L = loudness(y);
    if (Math.abs(target - L) < 0.05 || (peakBound && L < target)) break;
    g *= lin(target - L);
  }
  // Belt and braces.
  const tp = truePeak(y);
  if (tp > ceil) y = y.map((v) => (v * ceil) / tp);
  return { y, gr };
}

// ------------------------------------------------------------------ pipeline
function render(def) {
  const tr = new Track(def.ms / 1000, hashStr(def.name));
  def.draw(tr);
  let x = tr.x;
  if (def.rev) x = reverb(x, def.rev);
  // Speaker shaping: 4th-order Butterworth HPF at 250 Hz, gentle 2nd-order LPF at 7 kHz.
  new Biquad().highpass(HPF_HZ, 0.5412).run(x);
  new Biquad().highpass(HPF_HZ, 1.3066).run(x);
  new Biquad().lowpass(LPF_HZ, 0.707).run(x);
  let y = decimate2(x);
  // Edge fades: >= 2 ms in, `fade` ms out to exact zero.
  const fin = Math.round(0.002 * FS_OUT);
  const fout = Math.round(((def.fade ?? 30) / 1000) * FS_OUT);
  for (let i = 0; i < fin; i++) y[i] *= rc(i / fin);
  for (let i = 0; i < fout; i++) y[y.length - 1 - i] *= rc(i / fout);
  const target = def.target ?? TARGET_LUFS;
  const { y: z, gr } = normalise(y, target);
  const pcm = new Int16Array(z.length);
  for (let i = 0; i < z.length; i++) pcm[i] = Math.max(-32767, Math.min(32767, Math.round(z[i] * 32767)));
  pcm[0] = 0;
  pcm[pcm.length - 1] = 0;
  const f = Float64Array.from(pcm, (v) => v / 32767);
  return { pcm, lufs: loudness(f), tp: db(truePeak(f)), gr, target };
}

function wav(pcm) {
  const b = Buffer.alloc(44 + pcm.length * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + pcm.length * 2, 4); b.write('WAVE', 8);
  b.write('fmt ', 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22);
  b.writeUInt32LE(FS_OUT, 24); b.writeUInt32LE(FS_OUT * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34);
  b.write('data', 36); b.writeUInt32LE(pcm.length * 2, 40);
  Buffer.from(pcm.buffer, pcm.byteOffset, pcm.byteLength).copy(b, 44);
  return b;
}


export { render, wav };
