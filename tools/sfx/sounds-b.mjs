import {
  N, TAU, smooth, easeOut, envPerc, envNote, glide, ROUND, formant, tone,
  kalimba, marimba, bell, sparkle, bubble, bloop, noise,
} from './synth.mjs';
const SOUNDS = [];
const def = (d) => SOUNDS.push(d);


def({
  name: 'dizzy', ms: 1200, rev: { rt: 0.45, wet: 0.14 }, fade: 110,
  idea: 'slide whistle G6 -> C5 with a 6.5 Hz wobble and a breath, then a small twangy boing on G5',
  draw(tr) {
    const f0 = N('G6'), f1 = N('C5');
    const env = envNote(0.02, 0.72, 0.1);
    tone(tr, 0, 0.84, { f: glide(f0, f1, 0.02, 0.78, smooth), env, amp: 0.9, partials: [[1, 1], [2, 0.08]],
      vib: { rate: 6.5, depth: 0.045, delay: 0.05, fade: 0.15 } });
    noise(tr, 0, 0.84, { fc: glide(f0, f1, 0.02, 0.78, smooth), q: 3, env, amp: 0.08 });
    const g = N('G5');
    tone(tr, 0.88, 0.3, {
      f: (t) => g * (1 + 0.25 * Math.exp(-t / 0.03)) * (1 + 0.035 * Math.exp(-t / 0.12) * Math.sin(TAU * 14 * t)),
      env: envPerc(0.004, 0.07), amp: 0.9, partials: [[1, 1], [2, 0.1], [2.76, 0.15, 0.03]],
    });
  },
});

[['C5', 'C6'], ['G5', 'E6']].forEach(([a, b], i) =>
  def({
    name: `surprise_${i + 1}`, group: 'surprise', ms: 350, rev: { rt: 0.3, wet: 0.12 }, fade: 60,
    idea: `"whoop!" fast ease-out slide ${a}->${b} with a breath, settles with light vibrato`,
    draw(tr) {
      const env = envNote(0.01, 0.15, 0.15);
      const f = glide(N(a), N(b), 0, 0.12, easeOut);
      tone(tr, 0, 0.31, { f, env, amp: 1, partials: ROUND, colour: formant([[1000, 500, 1]]), vib: { rate: 7, depth: 0.01, delay: 0.14 } });
      noise(tr, 0, 0.2, { fc: f, q: 2, env: envPerc(0.01, 0.05), amp: 0.08 });
    },
  }));



// volume: one soft tick per level, pitch climbs the pentatonic.
['C6', 'D6', 'E6', 'G6', 'A6'].forEach((n, i) =>
  def({
    name: `volume_${i + 1}`, group: 'volume', ms: 90, rev: null, fade: 15,
    idea: `soft marimba tick on ${n} (level ${i + 1})`,
    draw(tr) {
      marimba(tr, 0, N(n), { tau: 0.022, bright: 0.7, a: 0.003 });
    },
  }));

def({
  name: 'hello', ms: 400, rev: { rt: 0.35, wet: 0.13 }, fade: 70,
  idea: '"hi!": quick G5 pickup then E6 scooped up from D6 with a friendly vibrato',
  draw(tr) {
    tone(tr, 0, 0.065, { f: N('G5'), env: envNote(0.006, 0.025, 0.034), amp: 0.75, partials: ROUND });
    tone(tr, 0.08, 0.26, {
      f: glide(N('D6'), N('E6'), 0, 0.04, easeOut), env: envNote(0.008, 0.12, 0.13), amp: 1,
      partials: ROUND, colour: formant([[1300, 600, 1]]), vib: { rate: 7, depth: 0.012, delay: 0.07 },
    });
  },
});

def({
  name: 'screen_on', ms: 130, max: 150, rev: { rt: 0.15, wet: 0.06 }, fade: 20,
  idea: 'tonal click pair up E6 -> A6 (short marimba ticks)',
  draw(tr) {
    marimba(tr, 0, N('E6'), { amp: 0.8, tau: 0.016, bright: 0.8 });
    marimba(tr, 0.04, N('A6'), { amp: 0.9, tau: 0.022, bright: 0.8 });
  },
});
def({
  name: 'screen_off', ms: 130, max: 150, rev: { rt: 0.15, wet: 0.06 }, fade: 20,
  idea: 'tonal click pair down A6 -> E6, second darker',
  draw(tr) {
    marimba(tr, 0, N('A6'), { amp: 0.8, tau: 0.016, bright: 0.8 });
    marimba(tr, 0.04, N('E6'), { amp: 0.9, tau: 0.022, bright: 0.4 });
  },
});

// Setup family: one motif (a rising G-C-E call answered by a bell) so the five
// moments of setup sound related. No voice in setup: the screen explains, these mark progress.
def({
  name: 'setup', ms: 1100, rev: { rt: 0.7, wet: 0.2 }, fade: 140,
  idea: '"come here": kalimba pickup G5-C6, bell E6, then a questioning bloop that lifts into A6',
  draw(tr) {
    kalimba(tr, 0, N('G5'), { amp: 0.6, tau: 0.22 });
    kalimba(tr, 0.09, N('C6'), { amp: 0.75, tau: 0.25 });
    bell(tr, 0.2, N('E6'), { amp: 0.7, tau: 0.35, index: 0.9, itau: 0.08, body: 0.5 });
    tone(tr, 0.46, 0.3, {
      f: glide(N('G6'), N('A6'), 0.02, 0.12, easeOut), env: envNote(0.012, 0.12, 0.14), amp: 0.55,
      partials: ROUND, colour: formant([[1400, 700, 1]]), vib: { rate: 6.5, depth: 0.01, delay: 0.1 },
    });
    sparkle(tr, 0.62, N('E7'), { amp: 0.12, tau: 0.08 });
  },
});

def({
  name: 'setup_phone', ms: 560, rev: { rt: 0.45, wet: 0.15 }, fade: 90,
  idea: '"found you!": the call answered, bright bloops C6->G6 plus a kalimba C7 and a glint',
  draw(tr) {
    bloop(tr, 0, N('C6'), { len: 0.07, amp: 0.7, rel: 0.04, partials: ROUND });
    bloop(tr, 0.075, N('G6'), { len: 0.1, amp: 0.9, rel: 0.06, from: 0.78, partials: ROUND });
    kalimba(tr, 0.16, N('C7'), { amp: 0.45, tau: 0.14, bright: 1.1 });
    sparkle(tr, 0.2, N('G7'), { amp: 0.14, tau: 0.07 });
  },
});

def({
  name: 'setup_wait', ms: 420, target: -27, rev: { rt: 0.5, wet: 0.2 }, fade: 110,
  idea: 'soft sonar ping on E6 (repeats every ~1.8 s while joining): a small bloop into a round bell',
  draw(tr) {
    bloop(tr, 0, N('E6'), { len: 0.05, from: 0.88, gtau: 0.01, amp: 0.35, rel: 0.03 });
    bell(tr, 0.008, N('E6'), { amp: 0.8, tau: 0.12, index: 0.6, itau: 0.04, body: 0.6 });
  },
});

def({
  name: 'setup_ok', ms: 1500, rev: { rt: 0.75, wet: 0.2 }, fade: 200,
  idea: 'small fanfare: kalimba run C5-E5-G5-C6-E6-G6, bell chord C6+E6+G6, sparkles, a last C7 bell',
  draw(tr) {
    ['C5', 'E5', 'G5', 'C6', 'E6', 'G6'].forEach((n, i) => kalimba(tr, i * 0.05, N(n), { amp: 0.5 + i * 0.06, tau: 0.14, bright: 1.1 }));
    for (const [n, a] of [['C6', 0.5], ['E6', 0.38], ['G6', 0.32]])
      bell(tr, 0.32, N(n), { amp: a, tau: 0.32, index: 1.1, itau: 0.08 });
    const hi = ['C7', 'D7', 'E7', 'G7', 'A7'];
    for (let k = 0; k < 7; k++) {
      const t = 0.36 + k * 0.07 + tr.rng() * 0.03;
      sparkle(tr, t, N(hi[Math.floor(tr.rng() * hi.length)]), { amp: 0.2 - k * 0.018, tau: 0.06 });
    }
    bell(tr, 0.86, N('C7'), { amp: 0.4, tau: 0.3, index: 0.8, itau: 0.06, body: 0.5 });
  },
});

def({
  name: 'setup_fail', ms: 760, rev: { rt: 0.45, wet: 0.15 }, fade: 120,
  idea: 'gentle "uh-oh": two rounded "oh" tones E6 then C6 that sags a little, soft marimba A4 under it',
  draw(tr) {
    const oh = [[750, 260, 1.1], [1100, 300, 0.6]];
    tone(tr, 0, 0.16, { f: N('E6'), env: envNote(0.01, 0.07, 0.07), amp: 0.8, partials: ROUND, colour: formant(oh) });
    tone(tr, 0.19, 0.5, {
      f: glide(N('C6'), N('C6') * 2 ** (-0.6 / 12), 0.12, 0.3), env: envNote(0.012, 0.18, 0.28), amp: 1,
      partials: ROUND, colour: formant(oh), vib: { rate: 5, depth: 0.008, delay: 0.15 },
    });
    marimba(tr, 0.19, N('A4'), { amp: 0.4, tau: 0.09, bright: 0.5 });
  },
});

// Interface family (settings menu, cards, power): short, tactile, wooden. Every touch that does something
// answers with its own sound; these stay small so a burst of them never gets tiring.
def({
  name: 'menu_open', ms: 240, max: 260, rev: { rt: 0.25, wet: 0.1 }, fade: 50,
  idea: 'a panel slides in and settles: soft rising air, marimba G5 -> C6, a small round bell on E6',
  draw(tr) {
    noise(tr, 0, 0.13, { fc: glide(900, 2600, 0, 0.13, easeOut), q: 1.0, env: envPerc(0.05, 0.04), amp: 0.2 });
    marimba(tr, 0.03, N('G5'), { amp: 0.6, tau: 0.03, bright: 0.7 });
    marimba(tr, 0.08, N('C6'), { amp: 0.8, tau: 0.04, bright: 0.8 });
    bell(tr, 0.09, N('E6'), { amp: 0.25, tau: 0.08, index: 0.5, itau: 0.03, body: 0.7 });
  },
});
def({
  name: 'menu_close', ms: 200, max: 220, rev: { rt: 0.2, wet: 0.08 }, fade: 45,
  idea: 'the panel folds away: falling air, marimba E6 -> G5, the second darker',
  draw(tr) {
    noise(tr, 0, 0.12, { fc: glide(2400, 800, 0, 0.12, easeOut), q: 1.0, env: envPerc(0.03, 0.04), amp: 0.18 });
    marimba(tr, 0.02, N('E6'), { amp: 0.7, tau: 0.03, bright: 0.7 });
    marimba(tr, 0.07, N('G5'), { amp: 0.8, tau: 0.04, bright: 0.4 });
  },
});
// Slider notches, 0..100 % in 10 % steps up the pentatonic: wood for the volume, glass for the light.
const NOTCH = ['C5', 'D5', 'E5', 'G5', 'A5', 'C6', 'D6', 'E6', 'G6', 'A6', 'C7'];
NOTCH.forEach((n, i) =>
  def({
    name: `detent_${i + 1}`, group: 'detent', ms: 60, max: 70, target: -25, rev: null, fade: 12,
    idea: `volume notch ${i * 10} %: tight wooden tick on ${n}`,
    draw(tr) {
      marimba(tr, 0, N(n), { tau: 0.014, bright: 0.9, a: 0.002 });
    },
  }));
NOTCH.forEach((n, i) =>
  def({
    name: `glint_${i + 1}`, group: 'glint', ms: 80, max: 90, target: -25, rev: { rt: 0.12, wet: 0.08 }, fade: 20,
    idea: `brightness notch ${i * 10} %: small glassy ping on ${n} with its octave`,
    draw(tr) {
      bell(tr, 0, N(n), { amp: 0.8, tau: 0.02, index: 0.7, itau: 0.01, body: 0.5, a: 0.002 });
      sparkle(tr, 0.004, N(n) * 2, { amp: 0.25, tau: 0.015 });
    },
  }));
def({
  name: 'arm', ms: 260, max: 280, rev: { rt: 0.2, wet: 0.08 }, fade: 50,
  idea: '"sure?": two low careful knocks A5 A5, the second held by a quiet bell on E6',
  draw(tr) {
    marimba(tr, 0, N('A5'), { amp: 0.8, tau: 0.025, bright: 0.5 });
    marimba(tr, 0.1, N('A5'), { amp: 0.85, tau: 0.035, bright: 0.5 });
    bell(tr, 0.1, N('E6'), { amp: 0.3, tau: 0.08, index: 0.8, itau: 0.03, body: 0.5 });
  },
});
def({
  name: 'disarm', ms: 170, max: 190, rev: { rt: 0.15, wet: 0.06 }, fade: 40,
  idea: 'never mind: a soft rounded A5 easing down to E5',
  draw(tr) {
    tone(tr, 0, 0.15, { f: glide(N('A5'), N('E5'), 0.01, 0.1, easeOut), env: envNote(0.006, 0.05, 0.08), amp: 0.8, partials: ROUND });
  },
});
def({
  name: 'deny', ms: 150, max: 170, rev: { rt: 0.12, wet: 0.05 }, fade: 30,
  idea: '"nuh-uh": two muted low syllables D5 then C5, closed vowel, very short',
  draw(tr) {
    const uh = [[500, 200, 1], [900, 250, 0.4]];
    tone(tr, 0, 0.05, { f: N('D5'), env: envNote(0.004, 0.02, 0.025), amp: 0.8, partials: ROUND, colour: formant(uh) });
    tone(tr, 0.065, 0.08, { f: N('C5'), env: envNote(0.004, 0.03, 0.045), amp: 0.9, partials: ROUND, colour: formant(uh) });
  },
});
def({
  name: 'page', ms: 110, max: 120, rev: { rt: 0.12, wet: 0.06 }, fade: 25,
  idea: 'a page flicks over: a short bright rustle and a light tick on G6',
  draw(tr) {
    noise(tr, 0, 0.07, { fc: glide(3200, 2000, 0, 0.07), q: 0.9, env: envPerc(0.008, 0.02), amp: 0.3 });
    marimba(tr, 0.03, N('G6'), { amp: 0.5, tau: 0.014, bright: 0.8 });
  },
});
def({
  name: 'dismiss', ms: 170, max: 190, rev: { rt: 0.15, wet: 0.07 }, fade: 40,
  idea: 'put away: air sliding down and a soft marimba C6 settling',
  draw(tr) {
    noise(tr, 0, 0.11, { fc: glide(2200, 700, 0, 0.11, easeOut), q: 1.0, env: envPerc(0.015, 0.035), amp: 0.2 });
    marimba(tr, 0.035, N('C6'), { amp: 0.75, tau: 0.03, bright: 0.5 });
  },
});
def({
  name: 'power_off', ms: 950, rev: { rt: 0.6, wet: 0.18 }, fade: 200,
  idea: 'goodnight: kalimba G6-E6-C6-G5 slowing down, a last soft round bell on C5',
  draw(tr) {
    [['G6', 0], ['E6', 0.09], ['C6', 0.2], ['G5', 0.34]].forEach(([n, t], i) =>
      kalimba(tr, t, N(n), { amp: 0.75 - i * 0.08, tau: 0.16 + i * 0.03, bright: 0.9 - i * 0.15 }));
    bell(tr, 0.5, N('C5'), { amp: 0.45, tau: 0.3, index: 0.5, itau: 0.06, body: 0.7 });
  },
});


export default SOUNDS;
