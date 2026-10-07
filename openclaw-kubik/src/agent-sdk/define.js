import { validateAdapter } from './adapter.js';

const MAX_REPLY_CHARS = 8192;
const MAX_REPLY_FRAGMENTS = 512;

// A reply returns text. The host queues it; playback continues after dispatch.
// isCurrent() becomes false when the user interrupts speech, but that must not
// cancel the agent. signal aborts only on close() or a new connect().
function report(callback, error) {
  if (typeof callback !== 'function') return;
  try { callback(error); } catch { /* reporting must not fail the turn twice */ }
}

function turnOpen(isCurrent) {
  try { return isCurrent?.() !== false; }
  catch { return false; }
}

function fragmentsOf(result) {
  if (typeof result === 'string') return [result];
  if (result == null) return [];
  const asyncItems = typeof result === 'object' && typeof result[Symbol.asyncIterator] === 'function';
  const items = asyncItems || (typeof result === 'object' && typeof result[Symbol.iterator] === 'function');
  if (items) return result;
  throw new Error('adapter reply must return text or text fragments');
}

async function speakFragments(turn, result, open, signal) {
  const source = fragmentsOf(result);
  const iterator = source[Symbol.asyncIterator]?.() ?? source[Symbol.iterator]();
  let completed = false;
  let characters = 0, fragments = 0;
  try {
    while (!signal.aborted) {
      const { done, value: part } = await iterator.next();
      if (signal.aborted) return;
      if (done) { completed = true; return; }
      if (typeof part !== 'string') throw new Error('adapter reply fragments must be text');
      characters += part.length;
      // Count empty fragments too: an unbounded producer must not monopolize the host.
      if (characters > MAX_REPLY_CHARS || ++fragments > MAX_REPLY_FRAGMENTS) {
        throw new Error('adapter reply exceeds 8192 characters or 512 fragments');
      }
      // Keep reading after interruption. Leaving the loop would stop the producer.
      if (!open() || !part.trim()) continue;
      await turn.speak(part);
    }
  } finally {
    if (!completed && typeof iterator.return === 'function') {
      try { await iterator.return(); } catch { /* preserve the reply or shutdown result */ }
    }
  }
}

function assertSpec(spec) {
  if (!spec || typeof spec !== 'object' || Array.isArray(spec)) throw new Error('agent adapter must be an object');
  if (typeof spec.connect !== 'function' || typeof spec.reply !== 'function') {
    throw new Error('agent adapter must implement connect() and reply()');
  }
  if (spec.dispatch !== undefined) throw new Error('implement reply(), not dispatch()');
  for (const hook of ['start', 'close', 'modelOptions', 'selectModel']) {
    if (spec[hook] !== undefined && typeof spec[hook] !== 'function') throw new Error(`agent adapter ${hook} must be a function`);
  }
}

function replaceSignal(state) {
  state.lifetime.abort();
  state.lifetime = new AbortController();
  return state.lifetime.signal;
}

async function connectSpec(spec, state, ctx) {
  const signal = replaceSignal(state);
  const attempt = state.lifetime;
  try { return await spec.connect({ ...ctx, signal }); }
  catch (error) {
    // Only this attempt may be replaced. close() or a newer connect already owns the slot.
    if (state.lifetime === attempt) attempt.abort();
    throw error;
  }
}

async function dispatchSpec(spec, state, turn) {
  const signal = state.lifetime.signal;
  const open = () => !signal.aborted && turnOpen(turn.isCurrent);
  if (!open()) return;
  try {
    const result = await spec.reply({
      device: turn.device, transcript: turn.transcript, turnId: turn.turnId, signal,
      isCurrent: () => turnOpen(turn.isCurrent), events: turn.events,
    });
    await speakFragments(turn, result, open, signal);
  } catch (error) {
    if (open()) report(turn.onAgentError, error);
  }
}

function assignOptional(adapter, spec) {
  if (spec.start) adapter.start = (ctx) => spec.start(ctx);
  if (spec.modelOptions) adapter.modelOptions = (device) => spec.modelOptions(device);
  if (spec.selectModel) adapter.selectModel = (device, id) => spec.selectModel(device, id);
}

/** Builds the host adapter. Authors implement connect and reply; the host calls dispatch. */
export function defineAdapter(spec) {
  assertSpec(spec);
  const state = { lifetime: new AbortController() };
  const adapter = {
    id: spec.id,
    label: spec.label,
    setup: spec.setup ?? [],
    connect(ctx = {}) { return connectSpec(spec, state, ctx); },
    dispatch(turn = {}) { return dispatchSpec(spec, state, turn); },
    async close() {
      state.lifetime.abort();
      await spec.close?.();
    },
  };
  assignOptional(adapter, spec);
  return validateAdapter(adapter);
}
