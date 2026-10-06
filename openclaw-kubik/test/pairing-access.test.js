import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import test from 'node:test';
import { guardSessionFrames } from '../src/pairing-access.js';

test('a cancel frame stays behind voice frames during a pending allow-list check', { timeout: 5000 }, async () => {
  const ws = new EventEmitter();
  ws.readyState = 1;
  const conn = { ws, phase: 'session' };
  const received = [];
  let release;
  let started;
  const authorizationStarted = new Promise((resolve) => { started = resolve; });
  const pendingAuthorization = new Promise((resolve) => { release = resolve; });
  let cancelProcessed;
  const cancelHandled = new Promise((resolve) => { cancelProcessed = resolve; });
  const session = {
    closed: false,
    ws,
    handleMessage: (message) => { received.push(`message:${message.t}:${message.on ?? ''}`); if (message.t === 'cancel') cancelProcessed(); },
    handleAudio: () => received.push('audio'),
  };

  const guard = guardSessionFrames(session, conn, {
    authorize: () => { started(); return pendingAuthorization; },
    onFailure: () => assert.fail('authorization failed'),
    onFrameError: (error) => assert.fail(error),
  });

  guard.message({ t: 'ptt', on: true, turn: 1 });
  await authorizationStarted;
  guard.audio({ pcm: Buffer.from([0, 0]) });
  guard.message({ t: 'ptt', on: false, turn: 1 });
  guard.message({ t: 'cancel' });
  assert.deepEqual(received, [], 'queued frames do not overtake the pending authorization');

  release(true);
  await cancelHandled;

  assert.deepEqual(received, ['message:ptt:true', 'audio', 'message:ptt:false', 'message:cancel:']);
});
