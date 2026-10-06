const PAIRING_TIMEOUT_MS = 5000;
const MAX_AUTH_QUEUE_BYTES = 64 * 1024;
const MAX_AUTH_QUEUE_FRAMES = 64;

/** Store calls share a deadline so a stalled store cannot retain an admission slot. */
export async function withPairingTimeout(operation, label) {
  let timer;
  try {
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`pairing ${label} timed out`)), PAIRING_TIMEOUT_MS);
      timer.unref?.();
    });
    return await Promise.race([Promise.resolve().then(operation), timeout]);
  } finally { clearTimeout(timer); }
}

/** Read the authoritative OpenClaw allow-list, with a bounded wait. */
export async function readAllowed(pairing) {
  const list = await withPairingTimeout(() => pairing.allowed(), 'allow-list read');
  return new Set((Array.isArray(list) ? list : []).map((entry) => String(entry).trim().toLowerCase()));
}

/**
 * Queues voice turns behind a fresh allow-list read, then resumes frames in order and within bounds.
 * Returns `{ message, audio }`: the only way frames reach the session while it is connected.
 */
export function guardSessionFrames(session, conn, { authorize, onFailure, onFrameError }) {
  const queue = [];
  let queuedBytes = 0, checking = false;
  const active = () => !session.closed && conn.phase === 'session' && session.ws.readyState === 1;
  const clear = () => { queue.length = 0; queuedBytes = 0; };
  const deliver = (frame) => { if (active()) { try { frame.run(); } catch (error) { onFrameError(error); } } };
  const drain = () => {
    while (!checking && queue.length && active()) {
      const frame = queue.shift(); queuedBytes -= frame.bytes;
      process(frame);
    }
  };
  const process = (frame) => {
    if (!active()) return;
    if (checking) {
      if (queue.length >= MAX_AUTH_QUEUE_FRAMES || frame.bytes > MAX_AUTH_QUEUE_BYTES - queuedBytes) {
        clear(); onFailure(); return;
      }
      queue.push(frame); queuedBytes += frame.bytes; return;
    }
    if (!frame.authorize) { deliver(frame); return; }
    checking = true;
    Promise.resolve().then(authorize).then((allowed) => {
      checking = false;
      if (!allowed || !active()) { clear(); return; }
      deliver(frame); drain();
    }).catch((error) => { checking = false; clear(); onFailure(error); });
  };
  conn.ws.once('close', clear);
  return {
    message: (value) => process({ authorize: value.t === 'ptt', bytes: Buffer.byteLength(JSON.stringify(value)), run: () => session.handleMessage(value) }),
    audio: (value) => process({ authorize: false, bytes: value.pcm.byteLength, run: () => session.handleAudio(value) }),
  };
}
