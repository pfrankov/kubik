// What OpenClaw is busy with right now, for Kubik's status indicator: every agent run in the Gateway (a
// voice turn, a heartbeat, a cron job, a subagent, a message from another channel) as seen on the runtime's
// agent event bus, reduced to the same categories Telegram's status reactions use.

// Same token lists (and order) as OpenClaw's status reactions (plugin-sdk/channel-feedback resolveToolEmoji).
const TOOL_CATEGORIES = [
  ['deploy', ['fastlane', 'deploy', 'upload', 'testflight', 'ship', 'release', 'publish', 'distribute']],
  ['build', ['build', 'compile', 'xcode', 'swift', 'gradle', 'cargo', 'make', 'cmake', 'webpack', 'vite', 'tsc', 'lint']],
  ['concierge', ['navigate', 'click', 'fill', 'screenshot', 'scroll', 'page', 'form', 'puppeteer', 'playwright', 'selenium', 'chromedp']],
  ['web', ['web_search', 'web-search', 'web_fetch', 'web-fetch', 'browser']],
  ['coding', ['exec', 'process', 'read', 'write', 'edit', 'session_status', 'bash']],
];

export function toolActivity(name) {
  const n = String(name ?? '').trim().toLowerCase();
  if (!n) return 'tool';
  for (const [category, tokens] of TOOL_CATEGORIES) if (tokens.some((token) => n.includes(token))) return category;
  return 'tool';
}

const STALL_MS = 30_000;       // no progress this long: shown as "stalled" (Telegram's hard stall)
const FORGET_MS = 15 * 60_000; // a run that never reported its end is dropped

/**
 * Tracks runs from `onAgentEvent` payloads ({ runId, stream, data, sessionKey, ts }). `onChange` is called
 * (debounced) whenever what should be shown may have changed; `summary(sessionKey)` says what to show to the
 * device whose own agent session is `sessionKey`.
 */
export class ActivityTracker {
  #runs = new Map();  // runId -> { sessionKey, activity, at, since }
  #timer = null;
  #sweep = null;
  #onChange;
  #debounceMs;
  #now;

  constructor({ onChange = () => {}, debounceMs = 300, now = Date.now } = {}) {
    this.#onChange = onChange;
    this.#debounceMs = debounceMs;
    this.#now = now;
    this.#sweep = setInterval(() => this.#tick(), 5000);
    this.#sweep.unref?.();
  }

  /** Feeds one agent event. */
  event(evt) {
    if (!evt || typeof evt.runId !== 'string') return;
    const data = evt.data ?? {};
    const now = this.#now();
    let run = this.#runs.get(evt.runId);
    if (evt.stream === 'lifecycle') {
      if (data.phase === 'end' || data.phase === 'error') {
        if (this.#runs.delete(evt.runId)) this.#changed();
        return;
      }
      if (data.phase !== 'start') return;
      if (!run) {
        this.#runs.set(evt.runId, run = { sessionKey: evt.sessionKey, activity: 'thinking', at: now, since: now });
        this.#changed();
      }
    }
    if (!run) return;  // events of a run that started before we subscribed (or already ended)
    if (evt.sessionKey && !run.sessionKey) run.sessionKey = evt.sessionKey;
    run.at = now;
    const next = activityOf(evt.stream, data, run.activity);
    if (next !== run.activity) {
      run.activity = next;
      this.#changed();
    }
  }

  /** { own, other }: the activity of the newest run in the device's own session, and of any other run. */
  summary(sessionKey) {
    const now = this.#now();
    let own = null, other = null;
    for (const run of this.#runs.values()) {
      const pick = { activity: now - run.at > STALL_MS ? 'stall' : run.activity, since: run.since };
      if (sessionKey && run.sessionKey === sessionKey) { if (!own || pick.since > own.since) own = pick; }
      else if (!other || pick.since > other.since) other = pick;
    }
    return { own: own?.activity ?? '', other: other?.activity ?? '' };
  }

  get size() { return this.#runs.size; }

  close() {
    clearTimeout(this.#timer);
    clearInterval(this.#sweep);
    this.#runs.clear();
  }

  #tick() {
    const now = this.#now();
    let changed = false;
    for (const [id, run] of this.#runs) {
      if (now - run.at > FORGET_MS) { this.#runs.delete(id); changed = true; }
      else if (now - run.at > STALL_MS && !run.stalled) { run.stalled = true; changed = true; }
      else if (now - run.at <= STALL_MS) run.stalled = false;
    }
    if (changed) this.#changed();
  }

  #changed() {
    if (this.#timer) return;
    this.#timer = setTimeout(() => { this.#timer = null; this.#onChange(); }, this.#debounceMs);
    this.#timer.unref?.();
  }
}

function activityOf(stream, data, current) {
  switch (stream) {
    case 'tool':
      if (data.phase === 'start') return toolActivity(data.name);
      if (data.phase === 'result' || data.phase === 'end') return 'thinking';
      return current;
    case 'compaction':
      return data.phase === 'start' ? 'compacting' : 'thinking';
    case 'assistant':
    case 'thinking':
      return current === 'compacting' ? current : 'thinking';
    default:
      return current;
  }
}
