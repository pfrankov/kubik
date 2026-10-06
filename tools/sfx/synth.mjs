const FS = 48000; // internal render rate (headroom for FM sidebands, clean decimation)
const FS_OUT = 24000; // what the firmware plays
const TAU = 2 * Math.PI;

// Global mix rules.
const TARGET_LUFS = -20;
const THINK_LUFS = -28; // think_* : "~8 dB quieter than the rest"
const CEIL_DBTP = -3;
const MAX_LIMIT_DB = 3; // at most this much transient gain reduction, otherwise just turn it down
const HPF_HZ = 180; // the speaker gives little below ~300 Hz: weight rides on the octave (see kalimba)
const LPF_HZ = 5000; // warm and round: a plush bear, not a squeaky toy

// ------------------------------------------------------------------ pitch
// Every note must belong to the major pentatonic, so any two sounds that
// overlap (or follow each other) stay consonant. KEY transposes the kit:
// 0 = C major pentatonic (C D E G A), 5 = F major pentatonic, ...
const KEY = 0;
// Kubik is a bear: the whole kit sits an octave below where a toy would squeak,
// and its "voice" (the vowel formants) is that of a bigger body.
const OCTAVE = -1;
const VOICE = 0.72; // formant scale
const DARK = 0.65; // band-passed noise centre scale (whooshes, purr)
const SEMI = { C: 0, D: 2, E: 4, G: 7, A: 9 };

function N(name) {
  const m = /^([A-G])(\d)$/.exec(name);
  if (!m || !(m[1] in SEMI)) throw new Error(`note ${name} is not in the pentatonic scale`);
  const midi = 12 * (+m[2] + 1 + OCTAVE) + SEMI[m[1]] + KEY;
  return 440 * 2 ** ((midi - 69) / 12);
}
// ------------------------------------------------------------------ rng
function mulberry32(a) {
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
function hashStr(s) {
  let h = 2166136261;
  for (const c of s) h = Math.imul(h ^ c.charCodeAt(0), 16777619);
  return h >>> 0;
}

// ------------------------------------------------------------------ envelopes
const clamp01 = (u) => (u <= 0 ? 0 : u >= 1 ? 1 : u);
const rc = (u) => 0.5 - 0.5 * Math.cos(Math.PI * clamp01(u)); // raised cosine 0..1
const smooth = (u) => rc(u);
const easeOut = (u) => 1 - (1 - clamp01(u)) ** 3;
const easeIn = (u) => clamp01(u) ** 2;

// Percussive: raised-cosine attack, exponential decay.
const envPerc = (a, tau) => (t) => {
  if (t < 0) return 0;
  if (t < a) return rc(t / a);
  return Math.exp(-(t - a) / tau);
};
// Sustained note: attack, hold (optionally slowly decaying), raised-cosine release to exact zero.
const envNote =
  (a, hold, rel, decay = Infinity) =>
  (t) => {
    if (t < 0) return 0;
    if (t < a) return rc(t / a);
    const v = Math.exp(-(t - a) / decay);
    const u = (t - a - hold) / rel;
    return u <= 0 ? v : u >= 1 ? 0 : v * (1 - rc(u));
  };
// Log-frequency glide f0 -> f1 between ts and ts + d.
const glide =
  (f0, f1, ts, d, shape = smooth) =>
  (t) =>
    f0 * (f1 / f0) ** shape((t - ts) / d);
// Scoop into a pitch: starts at f*from and settles exponentially.
const scoop = (f, from, tau) => (t) => f * from ** Math.exp(-t / tau);

// ------------------------------------------------------------------ track + voices
class Track {
  constructor(sec, seed) {
    this.n = Math.round(sec * FS);
    this.x = new Float64Array(this.n);
    this.rng = mulberry32(seed);
  }
}

// Partial presets: [ratio, amp, extra decay tau (s)].
const SINE = [[1, 1]];
const SOFT = [[1, 1], [2, 0.35]];
const ROUND = [[1, 1], [2, 0.3], [3, 0.1], [4, 0.04]];

// Formant weighting ("aw" / "oo" colour) applied per partial frequency.
const formant = (peaks) => (f) => {
  let g = 0.25;
  for (const [F, B, A] of peaks) g += A / (1 + ((f - F * VOICE) / (B * VOICE)) ** 2);
  return g;
};

// Generic additive oscillator: pitch function, envelope, partials, vibrato.
// A short taper at `len` guarantees the voice ends at exact zero even if the
// envelope was truncated.
function tone(tr, t0, len, o) {
  const { f, env, amp = 1, partials = SINE, vib = null, colour = null, taper = 0.008 } = o;
  const fFn = typeof f === 'function' ? f : () => f;
  const P = partials.map(([r, a, tau = Infinity]) => ({ r, a, tau }));
  const i0 = Math.round(t0 * FS);
  const n = Math.round(len * FS);
  let ph = 0; // cycles of the fundamental
  for (let k = 0; k < n; k++) {
    const i = i0 + k;
    if (i >= tr.n) break;
    const t = k / FS;
    let fr = fFn(t);
    if (vib) {
      const ramp = vib.delay ? rc((t - vib.delay) / (vib.fade ?? 0.08)) : 1;
      fr *= 1 + vib.depth * ramp * Math.sin(TAU * vib.rate * t);
    }
    let e = env(t) * amp;
    if (t > len - taper) e *= 1 - rc((t - (len - taper)) / taper);
    if (e !== 0) {
      let s = 0;
      for (const p of P) {
        const fp = fr * p.r;
        let a = p.a * (fp < 6500 ? 1 : 1 - rc((fp - 6500) / 1500));
        a *= Math.exp(-t / p.tau);
        if (colour) a *= colour(fp);
        s += a * Math.sin(TAU * ph * p.r);
      }
      tr.x[i] += e * s;
    }
    ph += fr / FS;
  }
}

// Kalimba-ish pluck: fundamental + inharmonic bar partials (2.76x, 5.4x) that die fast.
function kalimba(tr, t0, f, { amp = 1, tau = 0.35, bright = 1 } = {}) {
  const partials = [
    [1, 1],
    [2, 0.35, tau * 0.8], // the octave carries a low note the speaker cannot move air for
    [2.76, 0.3 * bright, tau * 0.2],
    [5.4, 0.1 * bright, tau * 0.07],
  ];
  tone(tr, t0, 0.003 + tau * 8, { f, env: envPerc(0.003, tau), amp, partials, taper: 0.02 });
}

// Marimba-ish soft mallet: 1x + 3.9x + 9.2x (upper ones very short).
function marimba(tr, t0, f, { amp = 1, tau = 0.12, bright = 1, a = 0.0025 } = {}) {
  const partials = [
    [1, 1],
    [2, 0.3, tau * 0.6],
    [3.93, 0.28 * bright, tau * 0.18],
    [9.2, 0.05 * bright, tau * 0.06],
  ];
  tone(tr, t0, a + tau * 8, { f, env: envPerc(a, tau), amp, partials, taper: 0.01 });
}

// Soft FM bell: carrier f, modulator f*ratio, index decaying so the attack
// is shimmery and the tail becomes a pure tone; plus a sine "body".
function bell(tr, t0, f, { amp = 1, tau = 0.5, ratio = 3.5, index = 1.5, itau = 0.12, body = 0.35, a = 0.003 } = {}) {
  const len = a + tau * 8;
  const i0 = Math.round(t0 * FS);
  const n = Math.round(len * FS);
  const env = envPerc(a, tau);
  const envB = envPerc(a * 2, tau * 1.3);
  const idx = f * ratio * (index + 1) > 9000 ? index * 0.4 : index; // tame very high bells
  for (let k = 0; k < n; k++) {
    const i = i0 + k;
    if (i >= tr.n) break;
    const t = k / FS;
    const I = idx * (0.1 + 0.9 * Math.exp(-t / itau));
    let s = (1 - body) * env(t) * Math.sin(TAU * f * t + I * Math.sin(TAU * f * ratio * t));
    s += body * envB(t) * Math.sin(TAU * f * t);
    if (t > len - 0.02) s *= 1 - rc((t - (len - 0.02)) / 0.02);
    tr.x[i] += amp * s;
  }
}

// Tiny glassy ping for sparkles.
function sparkle(tr, t0, f, { amp = 0.3, tau = 0.07 } = {}) {
  tone(tr, t0, 0.003 + tau * 8, { f, env: envPerc(0.003, tau), amp, partials: [[1, 1], [2.76, 0.18, tau * 0.3]], taper: 0.01 });
}

// Bubble: short sine whose pitch rises as it decays (like a real bubble).
function bubble(tr, t0, f, { amp = 1, dur = 0.045, rise = 0.5 } = {}) {
  const len = dur * 1.8;
  tone(tr, t0, len, {
    f: (t) => f * 2 ** (rise * (t / dur - 0.4)),
    env: envPerc(0.003, dur / 3),
    amp,
    partials: [[1, 1], [2, 0.06]],
    taper: len * 0.3,
  });
}

// Bloop: soft blip that slides into its note from `from` x the pitch.
function bloop(tr, t0, f, { len = 0.08, from = 0.82, gtau = 0.012, amp = 1, a = 0.004, rel = 0.04, partials = SOFT, vib } = {}) {
  tone(tr, t0, len, { f: scoop(f, from, gtau), env: envNote(a, Math.max(0, len - a - rel), rel), amp, partials, vib });
}

// Filtered noise, normalised to RMS ~0.35 before the envelope so `amp`
// is comparable with a tone of the same amp.
// `lp` adds a 4th-order low-pass on top (the band-pass skirts alone are only 12 dB/oct).
function noise(tr, t0, len, { fc, q = 1.2, env, amp = 1, am = null, stages = 2, lp = 0 } = {}) {
  const fcFn = typeof fc === 'function' ? (t) => fc(t) * DARK : () => fc * DARK;
  const n = Math.round(len * FS);
  const i0 = Math.round(t0 * FS);
  const buf = new Float64Array(n);
  const bq = Array.from({ length: stages }, () => new Biquad());
  const lps = lp ? [new Biquad().lowpass(lp, 0.5412), new Biquad().lowpass(lp, 1.3066)] : [];
  for (let k = 0; k < n; k++) {
    if (k % 16 === 0) for (const b of bq) b.bandpass(fcFn(k / FS), q);
    let v = tr.rng() * 2 - 1;
    for (const b of bq) v = b.tick(v);
    for (const b of lps) v = b.tick(v);
    buf[k] = v;
  }
  let ss = 0;
  for (const v of buf) ss += v * v;
  const g = 0.35 / Math.sqrt(ss / n + 1e-20);
  for (let k = 0; k < n; k++) {
    const i = i0 + k;
    if (i >= tr.n) break;
    const t = k / FS;
    let e = env(t) * amp * g;
    if (am) e *= am(t);
    if (t > len - 0.01) e *= 1 - rc((t - (len - 0.01)) / 0.01);
    tr.x[i] += e * buf[k];
  }
}

// Purr: pulse train of band-passed noise at ~25 Hz with a pitched hum under it.
function purr(tr, t0, len, { fc = 700, rate = 25, amp = 1, hum = N('C5'), humAmp = 0.35, breaths = 2 } = {}) {
  // Rate wobbles by +-1.5 Hz; phase is the closed-form integral so it's stateless.
  const phase = (t) => rate * t + (1.5 / (TAU * 0.9)) * (1 - Math.cos(TAU * 0.9 * t));
  const am = (t) => (0.5 - 0.5 * Math.cos(TAU * phase(t))) ** 3;
  // Breathing: `breaths` swells; the exhale is a bit louder and lower.
  const seg = len / breaths;
  const breath = (t) => {
    const j = Math.min(breaths - 1, Math.floor(t / seg));
    const u = (t - j * seg) / seg;
    return Math.sin(Math.PI * clamp01(u)) ** 0.8 * (j % 2 ? 1 : 0.8);
  };
  noise(tr, t0, len, { fc, q: 1.6, env: breath, am, amp, lp: fc * 2.2 }); // dark, no fizz
  tone(tr, t0, len, { f: hum, env: (t) => breath(t) * am(t), amp: amp * humAmp, partials: [[1, 1], [2, 0.5], [3, 0.15]] });
}

// ------------------------------------------------------------------ filters
class Biquad {
  constructor() {
    this.b0 = 1; this.b1 = 0; this.b2 = 0; this.a1 = 0; this.a2 = 0;
    this.z1 = 0; this.z2 = 0;
  }
  set(b0, b1, b2, a0, a1, a2) {
    this.b0 = b0 / a0; this.b1 = b1 / a0; this.b2 = b2 / a0; this.a1 = a1 / a0; this.a2 = a2 / a0;
    return this;
  }
  lowpass(f, q, fs = FS) {
    const w = (TAU * f) / fs, c = Math.cos(w), al = Math.sin(w) / (2 * q);
    return this.set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
  }
  highpass(f, q, fs = FS) {
    const w = (TAU * f) / fs, c = Math.cos(w), al = Math.sin(w) / (2 * q);
    return this.set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
  }
  bandpass(f, q, fs = FS) {
    const w = (TAU * f) / fs, c = Math.cos(w), al = Math.sin(w) / (2 * q);
    return this.set(al, 0, -al, 1 + al, -2 * c, 1 - al);
  }
  highshelf(f, q, gainDb, fs = FS) {
    const A = 10 ** (gainDb / 40), w = (TAU * f) / fs, c = Math.cos(w), al = Math.sin(w) / (2 * q), s = 2 * Math.sqrt(A) * al;
    return this.set(
      A * (A + 1 + (A - 1) * c + s), -2 * A * (A - 1 + (A + 1) * c), A * (A + 1 + (A - 1) * c - s),
      A + 1 - (A - 1) * c + s, 2 * (A - 1 - (A + 1) * c), A + 1 - (A - 1) * c - s,
    );
  }
  tick(x) {
    const y = this.b0 * x + this.z1;
    this.z1 = this.b1 * x - this.a1 * y + this.z2;
    this.z2 = this.b2 * x - this.a2 * y;
    return y;
  }
  run(x) {
    for (let i = 0; i < x.length; i++) x[i] = this.tick(x[i]);
    return x;
  }
}


export {
  FS, FS_OUT, TAU, TARGET_LUFS, THINK_LUFS, CEIL_DBTP, MAX_LIMIT_DB, HPF_HZ, LPF_HZ,
  N, hashStr, clamp01, rc, smooth, easeOut, easeIn, envPerc, envNote,
  glide, scoop, Track, SOFT, ROUND, formant, tone, kalimba, marimba,
  bell, sparkle, bubble, bloop, noise, purr, Biquad,
};
