// OpenClaw's cron jobs for Kubik's corner indicator: how many are running right now (script jobs too, which
// never show up as agent runs) and when the next one-shot job (a reminder) is due. Fed by the `cron_changed`
// plugin hook and the scheduler's own list; one watcher per process, shared through globalThis because the
// Gateway may load this module more than once.

const LIST_DEBOUNCE_MS = 500;
const RUNNING_FORGET_MS = 6 * 3600_000; // a start whose finish never came is dropped

export class CronWatcher {
  #jobs = new Map();     // id -> { enabled, kind, nextRunAtMs }
  #running = new Map();  // id -> since (ms)
  #listeners = new Set();
  #getCron = null;
  #timer = null;
  #listing = null;
  #last = '0|-1';  // nothing running, nothing due: what a device shows without being told
  #now;

  constructor({ now = Date.now } = {}) { this.#now = now; }

  /** The Gateway's cron service (hook context `getCron`), for listing jobs. */
  setSource(getCron) {
    if (typeof getCron === 'function') this.#getCron = getCron;
  }

  /** A `cron_changed` hook event. */
  event(evt, ctx) {
    if (!evt || typeof evt.jobId !== 'string') return;
    this.setSource(ctx?.getCron);
    const id = evt.jobId;
    if (evt.action === 'removed') { this.#jobs.delete(id); this.#running.delete(id); }
    else {
      if (evt.job) this.#remember(evt.job);
      if (evt.action === 'started') this.#running.set(id, this.#now());
      if (evt.action === 'finished') this.#running.delete(id);
      if (evt.nextRunAtMs !== undefined && this.#jobs.has(id)) this.#jobs.get(id).nextRunAtMs = evt.nextRunAtMs;
    }
    this.#changed();
    this.refresh();
  }

  /** Re-reads the job list (debounced). */
  refresh() {
    if (!this.#getCron || this.#timer) return;
    this.#timer = setTimeout(() => { this.#timer = null; this.#list(); }, LIST_DEBOUNCE_MS);
    this.#timer.unref?.();
  }

  async #list() {
    if (this.#listing) return;
    let cron;
    try { cron = this.#getCron?.(); } catch { cron = undefined; }
    if (!cron?.list) return;
    this.#listing = (async () => {
      try {
        const result = await cron.list({ includeDisabled: false });
        const jobs = Array.isArray(result) ? result : result?.jobs ?? [];
        this.#jobs.clear();
        for (const job of jobs) {
          this.#remember(job);
          if (job.state?.runningAtMs && !this.#running.has(job.id)) this.#running.set(job.id, job.state.runningAtMs);
        }
        this.#changed();
      } catch { /* the next event lists again */ }
      finally { this.#listing = null; }
    })();
    await this.#listing;
  }

  #remember(job) {
    if (!job || typeof job.id !== 'string') return;
    this.#jobs.set(job.id, { enabled: job.enabled !== false, kind: job.schedule?.kind, nextRunAtMs: job.state?.nextRunAtMs });
  }

  /** { running: jobs running now, next: seconds until the next one-shot job (-1 = none) }. */
  summary() {
    const now = this.#now();
    for (const [id, since] of this.#running) if (now - since > RUNNING_FORGET_MS) this.#running.delete(id);
    let next = Infinity;
    for (const [id, job] of this.#jobs) {
      if (!job.enabled || job.kind !== 'at' || this.#running.has(id)) continue;
      if (Number.isFinite(job.nextRunAtMs) && job.nextRunAtMs > now) next = Math.min(next, job.nextRunAtMs);
    }
    return { running: this.#running.size, next: next === Infinity ? -1 : Math.ceil((next - now) / 1000) };
  }

  /** Calls `listener(summary)` whenever the summary may have changed; returns an unsubscribe function. */
  subscribe(listener) {
    this.#listeners.add(listener);
    return () => this.#listeners.delete(listener);
  }

  #changed() {
    const summary = this.summary();
    // A countdown moves every second by itself; only the due time or the running count are news.
    const key = `${summary.running}|${summary.next < 0 ? -1 : Math.round((this.#now() / 1000 + summary.next) / 5)}`;
    if (key === this.#last) return;
    this.#last = key;
    for (const listener of this.#listeners) { try { listener(summary); } catch { /* one bad listener */ } }
  }
}

const REGISTRY = Symbol.for('openclaw.kubik.cron');
/** The process-wide watcher. */
export const cronWatcher = () => (globalThis[REGISTRY] ??= new CronWatcher());

/** Plugin registration: feeds the watcher from the Gateway's cron hooks (full registration mode only). */
export function registerCronHooks(api) {
  if (typeof api?.on !== 'function') return;
  const watcher = cronWatcher();
  const list = (_event, ctx) => { watcher.setSource(ctx?.getCron); watcher.refresh(); };
  api.on('gateway_start', list);
  api.on('cron_reconciled', list);
  api.on('cron_changed', (event, ctx) => watcher.event(event, ctx));
}
