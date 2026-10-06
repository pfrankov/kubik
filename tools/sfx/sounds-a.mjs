import {
  N, THINK_LUFS, easeOut, easeIn, envPerc, envNote, glide, scoop, SOFT, ROUND,
  formant, tone, kalimba, marimba, bell, sparkle, bubble, bloop, noise, purr,
} from './synth.mjs';
// Each sound: name, ms (total length incl. tail), optional max (hard limit),
// rev (reverb), target (LUFS), fade (ms), idea (one line), draw(tr).
const SOUNDS = [];
const def = (d) => SOUNDS.push(d);

def({
  name: 'boot', ms: 1200, rev: { rt: 0.7, wet: 0.18 }, fade: 160,
  idea: 'kalimba C5-E5-G5-C6 rising, soft FM bell E6 with a faint C7 sparkle on top',
  draw(tr) {
    [['C5', 0, 0.75], ['E5', 0.1, 0.8], ['G5', 0.2, 0.85], ['C6', 0.32, 0.9]].forEach(([n, t, a]) =>
      kalimba(tr, t, N(n), { amp: a, tau: 0.3 }));
    bell(tr, 0.45, N('E6'), { amp: 0.55, tau: 0.28, index: 1.3, itau: 0.1 });
    sparkle(tr, 0.52, N('C7'), { amp: 0.16, tau: 0.09 });
  },
});

// listen_start: two rising bloops, <= 160 ms total, nearly dry (mic opens right after).
[['G5', 'C6'], ['A5', 'D6'], ['G5', 'D6']].forEach(([a, b], i) =>
  def({
    name: `listen_start_${i + 1}`, group: 'listen_start', ms: 150, max: 160, rev: { rt: 0.12, wet: 0.06 }, fade: 12,
    idea: `rising bloop pair ${a}->${b}, each slides up into its note; dry so the mic opens clean`,
    draw(tr) {
      bloop(tr, 0, N(a), { len: 0.065, amp: 0.75, rel: 0.035 });
      bloop(tr, 0.05, N(b), { len: 0.09, amp: 1, rel: 0.05, from: 0.8 });
    },
  }));

// listen_stop: descending bloop pair + a tiny airy whoosh.
[['C6', 'G5'], ['D6', 'A5'], ['D6', 'G5']].forEach(([a, b], i) =>
  def({
    name: `listen_stop_${i + 1}`, group: 'listen_stop', ms: 280, max: 300, rev: { rt: 0.25, wet: 0.1 }, fade: 30,
    idea: `falling bloop pair ${a}->${b} (each slides down) + band-passed noise whoosh sweeping 3.5->1.4 kHz`,
    draw(tr) {
      bloop(tr, 0, N(a), { len: 0.07, from: 1.12, amp: 0.85, rel: 0.035 });
      bloop(tr, 0.06, N(b), { len: 0.12, from: 1.12, amp: 1, rel: 0.07 });
      noise(tr, 0.05, 0.2, { fc: glide(3500, 1400, 0, 0.2), q: 1.1, env: envPerc(0.03, 0.05), amp: 0.22 });
    },
  }));

def({
  name: 'latch_start', ms: 175, max: 180, rev: { rt: 0.12, wet: 0.06 }, fade: 15,
  idea: 'double tap: two quick marimba ticks E6-E6, the second blooms into a rising G5->C6 bloop',
  draw(tr) {
    marimba(tr, 0, N('E6'), { amp: 0.55, tau: 0.02 });
    marimba(tr, 0.055, N('E6'), { amp: 0.6, tau: 0.02 });
    bloop(tr, 0.055, N('C6'), { len: 0.11, from: 0.75, gtau: 0.015, amp: 0.8, rel: 0.055 });
  },
});

// think: quiet bubble pops.
[
  [['A5', 0.02, 1]],
  [['E6', 0.01, 1], ['C6', 0.1, 0.7]],
  [['G5', 0.01, 0.9], ['D6', 0.07, 0.55], ['A5', 0.14, 0.75]],
].forEach((pops, i) =>
  def({
    name: `think_${i + 1}`, group: 'think', ms: i === 0 ? 200 : 240, max: 250, target: THINK_LUFS,
    rev: { rt: 0.3, wet: 0.18 }, fade: 40,
    idea: `${pops.length} soft bubble pop(s) ${pops.map((p) => p[0]).join(' ')}, pitch rising inside each pop; very quiet`,
    draw(tr) {
      for (const [n, t, a] of pops) bubble(tr, t, N(n), { amp: a, dur: 0.04, rise: 0.45 });
    },
  }));

def({
  name: 'notify', ms: 1100, rev: { rt: 0.8, wet: 0.18 }, fade: 150,
  idea: 'three FM bells G5-C6-E6 (ratio 3.5, index decaying) doubled by quiet kalimba for a friendly pluck',
  draw(tr) {
    [['G5', 0, 0.3], ['C6', 0.14, 0.3], ['E6', 0.28, 0.42]].forEach(([n, t, tau], i) => {
      bell(tr, t, N(n), { amp: 0.8 + i * 0.05, tau, index: 1.4, itau: 0.09 });
      kalimba(tr, t, N(n), { amp: 0.3, tau: 0.15, bright: 0.6 });
    });
  },
});

// error: rounded "uh-oh", falling minor third, ends with a little droop.
[['G5', 'E5'], ['C6', 'A5']].forEach(([a, b], i) =>
  def({
    name: `error_${i + 1}`, group: 'error', ms: 500, rev: { rt: 0.35, wet: 0.12 }, fade: 60,
    idea: `vowel-coloured "uh-oh" ${a}->${b} (minor third down), second note droops 1.5 semitones with soft vibrato`,
    draw(tr) {
      const col = formant([[700, 350, 1], [1150, 300, 0.6]]);
      tone(tr, 0, 0.16, { f: scoop(N(a), 0.95, 0.02), env: envNote(0.015, 0.09, 0.055), amp: 0.9, partials: ROUND, colour: col });
      tone(tr, 0.19, 0.29, {
        f: (t) => scoop(N(b), 0.96, 0.02)(t) * glide(1, 2 ** (-1.5 / 12), 0.12, 0.16)(t),
        env: envNote(0.018, 0.12, 0.15), amp: 1, partials: ROUND, colour: col,
        vib: { rate: 5.5, depth: 0.007, delay: 0.06 },
      });
    },
  }));

// not_heard: "hm?" - soft short note, then a question-like upward chirp.
def({
  name: 'not_heard_1', group: 'not_heard', ms: 350, rev: { rt: 0.3, wet: 0.12 }, fade: 50,
  idea: 'one soft "hm" on A5 that curls up to D6 at the end like a question',
  draw(tr) {
    tone(tr, 0, 0.3, {
      f: glide(N('A5'), N('D6'), 0.12, 0.13, easeIn), env: envNote(0.02, 0.19, 0.09), amp: 1,
      partials: ROUND, colour: formant([[900, 400, 1]]), vib: { rate: 6, depth: 0.006, delay: 0.05 },
    });
  },
});
def({
  name: 'not_heard_2', group: 'not_heard', ms: 350, rev: { rt: 0.3, wet: 0.12 }, fade: 50,
  idea: 'short G5 "h-" then a C6->E6 upward chirp "m?"',
  draw(tr) {
    tone(tr, 0, 0.08, { f: N('G5'), env: envNote(0.012, 0.03, 0.038), amp: 0.7, partials: ROUND });
    tone(tr, 0.1, 0.22, {
      f: glide(N('C6'), N('E6'), 0.03, 0.1, easeOut), env: envNote(0.012, 0.1, 0.1), amp: 1,
      partials: ROUND, vib: { rate: 7, depth: 0.008, delay: 0.1 },
    });
  },
});

def({
  name: 'connect', ms: 600, rev: { rt: 0.45, wet: 0.15 }, fade: 80,
  idea: 'bright kalimba arpeggio C6-E6-G6-C7 at 60 ms steps with a glassy E7 sparkle',
  draw(tr) {
    ['C6', 'E6', 'G6', 'C7'].forEach((n, i) => kalimba(tr, i * 0.06, N(n), { amp: 0.7 + i * 0.07, tau: 0.13 + i * 0.02, bright: 1.2 }));
    sparkle(tr, 0.24, N('E7'), { amp: 0.25, tau: 0.08 });
  },
});

def({
  name: 'disconnect', ms: 500, rev: { rt: 0.4, wet: 0.14 }, fade: 80,
  idea: 'two muted kalimba notes E6->A5, the second sags 30 cents like power running out',
  draw(tr) {
    kalimba(tr, 0, N('E6'), { amp: 0.8, tau: 0.12, bright: 0.5 });
    const f = N('A5');
    tone(tr, 0.15, 0.34, {
      f: glide(f, f * 2 ** (-0.3 / 12), 0.03, 0.25), env: envPerc(0.004, 0.1), amp: 1,
      partials: [[1, 1], [2, 0.05], [2.76, 0.12, 0.03]],
    });
  },
});

def({
  name: 'wake', ms: 800, rev: { rt: 0.5, wet: 0.15 }, fade: 90,
  idea: 'yawn: breathy C5->A5 glide swelling up with growing vibrato, then C7-E7-G7 sparkles',
  draw(tr) {
    const env = envNote(0.12, 0.25, 0.14);
    tone(tr, 0, 0.51, {
      f: glide(N('C5'), N('A5'), 0.04, 0.4), env, amp: 1, partials: ROUND,
      colour: formant([[800, 400, 1]]), vib: { rate: 5, depth: 0.012, delay: 0.15, fade: 0.25 },
    });
    noise(tr, 0, 0.51, { fc: glide(900, 1600, 0.04, 0.4), q: 1.5, env, amp: 0.12 });
    [['C7', 0.47], ['E7', 0.53], ['G7', 0.59]].forEach(([n, t], i) => sparkle(tr, t, N(n), { amp: 0.28 - i * 0.04, tau: 0.08 }));
  },
});

// tap: six little creature chirps with different pitch contours.
const TAPS = [
  { ms: 90, idea: '"bip!" short C6 sine blip with a tiny upward scoop', draw(tr) {
    bloop(tr, 0, N('C6'), { len: 0.07, from: 0.9, gtau: 0.008, rel: 0.04, partials: [[1, 1], [2, 0.18]] });
  } },
  { ms: 150, idea: '"bwip?" G5->D6 upward swoop', draw(tr) {
    tone(tr, 0, 0.13, { f: glide(N('G5'), N('D6'), 0.005, 0.07, easeOut), env: envNote(0.006, 0.06, 0.06), partials: SOFT });
  } },
  { ms: 100, idea: '"pik" tight marimba tick E6', draw(tr) {
    marimba(tr, 0, N('E6'), { tau: 0.028, bright: 1.2 });
  } },
  { ms: 160, idea: '"boop" A5->E5 downward glide', draw(tr) {
    tone(tr, 0, 0.14, { f: glide(N('A5'), N('E5'), 0.01, 0.08, easeOut), env: envNote(0.006, 0.06, 0.07), partials: ROUND });
  } },
  { ms: 150, idea: '"bi-dip" two micro blips C6 then G6', draw(tr) {
    bloop(tr, 0, N('C6'), { len: 0.045, rel: 0.025, amp: 0.8 });
    bloop(tr, 0.055, N('G6'), { len: 0.08, rel: 0.05, from: 0.88 });
  } },
  { ms: 210, idea: '"bwee-oo" D6 up to G6 and back down to E6 with vibrato', draw(tr) {
    const f = (t) => (t < 0.07 ? glide(N('D6'), N('G6'), 0, 0.07)(t) : glide(N('G6'), N('E6'), 0.07, 0.09)(t));
    tone(tr, 0, 0.19, { f, env: envNote(0.008, 0.1, 0.08), partials: SOFT, vib: { rate: 11, depth: 0.012, delay: 0.06 } });
  } },
];
TAPS.forEach((t, i) =>
  def({ name: `tap_${i + 1}`, group: 'tap', ms: t.ms, max: 220, rev: { rt: 0.2, wet: 0.08 }, fade: 20, idea: t.idea, draw: t.draw }));

// giggle: descending staccato syllables, each with a quick vibrato wobble.
[
  { notes: ['G6', 'E6', 'D6', 'C6', 'A5'], step: [0, 0.075, 0.15, 0.225, 0.3] },
  { notes: ['E6', 'D6', 'C6', 'A5'], step: [0, 0.065, 0.13, 0.195] },
  { notes: ['D6', 'C6', 'A5', 'G5', 'E5'], step: [0, 0.07, 0.13, 0.2, 0.26] },
].forEach((g, i) =>
  def({
    name: `giggle_${i + 1}`, group: 'giggle', ms: 500, rev: { rt: 0.3, wet: 0.12 }, fade: 60,
    idea: `"hehehe" ${g.notes.join('-')} staccato, each syllable scooped and wobbled at 16 Hz`,
    draw(tr) {
      g.notes.forEach((n, k) => {
        const last = k === g.notes.length - 1;
        const len = last ? 0.11 : 0.05;
        tone(tr, g.step[k], len, {
          f: scoop(N(n), 1.06, 0.01), env: envNote(0.005, len * 0.35, len * 0.55),
          amp: 1 - k * 0.07, partials: ROUND, colour: formant([[1200, 600, 1]]),
          vib: { rate: 16, depth: 0.025 },
        });
      });
    },
  }));

// pet: purr + sparkle.
[
  { fc: 650, rate: 25, hum: 'C5', sp: [['E7', 0.72], ['G7', 0.84]] },
  { fc: 820, rate: 23, hum: 'G5', sp: [['C7', 0.5], ['A6', 0.95]] },
].forEach((p, i) =>
  def({
    name: `pet_${i + 1}`, group: 'pet', ms: 1300, rev: { rt: 0.4, wet: 0.1 }, fade: 120,
    idea: `purr: ${p.rate} Hz pulses of noise at ${p.fc} Hz + hum on ${p.hum} (heard via harmonics), two breaths, sparkles ${p.sp.map((s) => s[0]).join(' ')}`,
    draw(tr) {
      purr(tr, 0, 1.28, { fc: p.fc, rate: p.rate, hum: N(p.hum), amp: 1 });
      for (const [n, t] of p.sp) sparkle(tr, t, N(n), { amp: 0.22, tau: 0.1 });
    },
  }));


export default SOUNDS;
