// Physical acceptance: quiet animation is strict; finite reactions retain the measured pre-refactor budget.
export const FRAME_LIMITS = { minFps: 29.5, maxLateFrames: 0, transientMs: 41, shownGapMs: 56 };
const number = (text, pattern) => Number(pattern.exec(text)?.[1]);
export function parseStats(text) {
  return {
    fps: number(text, /([\d.]+) fps/), frames: number(text, /\((\d+) frames\)/),
    meanMs: number(text, /(\d+) us\/frame \(max/) / 1000, maxMs: number(text, /max (\d+),/) / 1000,
    late: number(text, /(\d+) over/), px: number(text, /(\d+) px\/frame/), prims: number(text, /prims (\d+)/),
    face: number(text, /face (\d+)/), shapes: number(text, /shapes (\d+)/), hash: number(text, /hash (\d+)/),
    prep: number(text, /prep (\d+)/), image: number(text, /image (\d+)/), glass: number(text, /glass (\d+)/),
    send: number(text, /send (\d+)\)/), character: /character (\w+)/.exec(text)?.[1],
    gapMs: number(text, /frames shown \d+\.\.(\d+) us apart/) / 1000,
    slow: text.split('\n').filter((l) => /slow frame/.test(l)).map((l) => l.replace(/^.*main: /, '')),
  };
}
export function frameVerdict(s, kind = 'stable') {
  if (!['fps', 'frames', 'meanMs', 'maxMs', 'late', 'gapMs'].every((key) => Number.isFinite(s[key]) && s[key] >= 0)) return false;
  if (kind === 'stable') return s.frames >= 15 && s.fps >= FRAME_LIMITS.minFps && s.late === 0;
  const budgets = { onset: [8, 6], release: [20, 4], reaction: [60, 2], conversation: [60, 5] };
  const budget = budgets[kind];
  return !!budget && s.frames >= budget[0] && s.late <= budget[1] &&
    s.meanMs <= 1000 / 30 && s.maxMs <= FRAME_LIMITS.transientMs && s.gapMs <= FRAME_LIMITS.shownGapMs &&
    (kind === 'onset' || kind === 'release' || s.fps >= FRAME_LIMITS.minFps);
}
