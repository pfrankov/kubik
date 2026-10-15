// Physical acceptance: quiet animation is strict; finite reactions retain the measured pre-refactor budget.
export const FRAME_LIMITS = { minFps: 29.5, maxLateFrames: 0, transientMs: 41, shownGapMs: 56 };
function measuredWindow(s) {
  return s.presented === s.frames && s.overflows === 0 &&
    ['fps', 'frames', 'meanMs', 'maxMs', 'late', 'gapMs'].every((key) => Number.isFinite(s[key]) && s[key] >= 0);
}
export function frameVerdict(s, kind = 'stable') {
  if (!measuredWindow(s)) return false;
  if (kind === 'stable') return s.frames >= 15 && s.fps >= FRAME_LIMITS.minFps && s.late === 0;
  const budgets = { onset: [8, 6], release: [20, 4], reaction: [60, 2], conversation: [60, 5] };
  const budget = budgets[kind];
  return !!budget && s.frames >= budget[0] && s.late <= budget[1] &&
    s.meanMs <= 1000 / 30 && s.maxMs <= FRAME_LIMITS.transientMs && s.gapMs <= FRAME_LIMITS.shownGapMs &&
    (kind === 'onset' || kind === 'release' || s.fps >= FRAME_LIMITS.minFps);
}
