#!/usr/bin/env node
// Device self-test server: speaks each utterance back (IMA ADPCM, as the real server does) to exercise mic, link,
// speaker and face states.
// Run with: node parrot-server.mjs [port] [replyDelayMs=900] (acceptance uses 1800 for a real thinking sample)
import { WebSocketServer } from 'ws';
import { randomBytes, verify } from 'node:crypto';
import { authMessage, decodeDeviceKey, DEVICE_PATH, ImaEncoder, parseAudioFrame, parseDeviceMessage, speechFrameIma } from '../../openclaw-kubik/src/protocol.js';

const port = Number(process.argv[2] || 18999);
const replyDelayMs = Number(process.argv[3] || 900);
if (!Number.isInteger(replyDelayMs) || replyDelayMs < 900 || replyDelayMs > 5000) throw Error('reply delay must be 900..5000 ms');
const wss = new WebSocketServer({ host: '127.0.0.1', port, path: DEVICE_PATH });
wss.on('listening', () => console.log(`parrot on ws://127.0.0.1:${port}${DEVICE_PATH}`));

function sendJson(ws, message) {
  ws.send(JSON.stringify(message));
}

function newSession() {
  return { pcm: [], bytes: 0, turn: null, speaking: false, generation: 0, timer: null, replyTimer: null,
    hello: null, nonce: null, publicKey: null, authorized: false };
}

function parseMessage(ws, data) {
  try { return parseDeviceMessage(data); }
  catch { ws.close(4002, 'invalid frame'); return null; }
}

function recordAudio(session, ws, data) {
  if (!session.authorized) {
    ws.close(4002, 'authenticate first');
    return;
  }
  let frame;
  try { frame = parseAudioFrame(data); }
  catch { ws.close(4002, 'invalid microphone frame'); return; }
  if (frame.turn === session.turn) {
    if (session.bytes + frame.pcm.length > 48000 * 61) { ws.close(4002, 'recording limit'); return; }
    session.bytes += frame.pcm.length; session.pcm.push(frame.pcm);
  }
}

function acceptHello(session, ws, message, send) {
  if (session.hello) {
    ws.close(4002, 'duplicate hello');
    return;
  }
  session.hello = message;
  session.publicKey = decodeDeviceKey(message.key).publicKey;
  session.nonce = randomBytes(32).toString('base64');
  send({ t: 'challenge', nonce: session.nonce });
}

function verifyDevice(session, ws, message, send) {
  let valid = false;
  try {
    if (session.hello && !session.authorized) {
      valid = verify('sha256', authMessage({
        nonce: session.nonce,
        device: session.hello.device,
        key: session.hello.key,
        bind: 'none', // plain ws:// self-test server
      }), session.publicKey, Buffer.from(message.sig, 'base64'));
    }
  } catch { /* malformed signature */ }
  if (!valid) {
    ws.close(4001, 'bad signature');
    return;
  }
  session.authorized = true;
  send({ t: 'welcome', session: 'parrot' });
  send({ t: 'capabilities', stt: { available: true }, tts: { available: true } });
}

function handleMessage(session, ws, data, binary, send) {
  if (binary) return recordAudio(session, ws, data);
  const message = parseMessage(ws, data);
  if (!message) return;
  if (message.t === 'hello') return acceptHello(session, ws, message, send);
  if (message.t === 'auth') return verifyDevice(session, ws, message, send);
  if (!session.authorized) {
    ws.close(4002, 'authenticate first');
    return;
  }
  handleAuthorizedMessage(session, ws, message, send);
}

function stopReply(session) {
  clearInterval(session.timer); clearTimeout(session.replyTimer);
  session.timer = session.replyTimer = null; session.speaking = false;
}

function cancelTurn(session, message) {
    if (message.turn === undefined) { stopReply(session); session.turn = null; session.pcm = []; }
    else if (message.turn === session.turn) { session.turn = null; session.pcm = []; }
}

function handleAuthorizedMessage(session, ws, message, send) {
  if (message.t !== 'ping') console.log('dev:', JSON.stringify(message));
  if (message.t === 'ptt' && message.on) {
    if (message.automatic && (session.turn !== null || session.speaking || session.replyTimer)) {
      send({ t: 'error', code: 'busy' }); return;
    }
    session.turn = message.turn;
    session.pcm = []; session.bytes = 0;
    stopReply(session);
  }
  if (message.t === 'cancel') cancelTurn(session, message);
  if (message.t === 'ptt' && !message.on && message.turn === session.turn) {
    session.turn = null; finishUtterance(session, ws, send);
  }
  if (message.t === 'played') { session.speaking = false; send({ t: 'state', s: 'idle' }); }
}

function audioPeak(audio) {
  let peak = 0;
  for (let offset = 0; offset + 1 < audio.length; offset += 2) {
    peak = Math.max(peak, Math.abs(audio.readInt16LE(offset)));
  }
  return peak;
}

function finishUtterance(session, ws, send) {
  const audio = Buffer.concat(session.pcm);
  console.log(`utterance ${(audio.length / 48000).toFixed(2)} s, peak ${audioPeak(audio)}`);
  if (audio.length < 48000 * 0.3) return send({ t: 'error', code: 'stt_empty' });
  send({ t: 'state', s: 'thinking' });
  clearTimeout(session.replyTimer);
  session.replyTimer = setTimeout(() => startReply(session, ws, audio, send), replyDelayMs);
}

function startReply(session, ws, audio, send, kind = 'reply') {
  session.replyTimer = null; session.speaking = true;
  session.generation = (session.generation + 1) & 0xff;
  const generation = session.generation;
  send({ t: 'emotion', e: ['happy', 'love', 'proud', 'wink', 'surprised'][generation % 5], ms: 5000 });
  send({ t: 'speak', gen: generation, kind });
  send({ t: 'state', s: 'speaking' });

  const stream = { offset: 0, startedAt: Date.now(), generation, encoder: new ImaEncoder() };
  session.timer = setInterval(() => streamReply(session, ws, audio, stream, send), 50);
}

function streamReply(session, ws, audio, stream, send) {
  const chunkSize = 4800; // 100 ms
  while (stream.offset < audio.length && (stream.offset / 48) - (Date.now() - stream.startedAt) < 600) {
    const chunk = audio.subarray(stream.offset, stream.offset + chunkSize);
    ws.send(speechFrameIma(stream.generation, stream.encoder.encode(chunk)));
    stream.offset += chunkSize;
  }
  if (stream.offset >= audio.length) {
    clearInterval(session.timer);
    send({ t: 'speak_end', gen: stream.generation });
  }
}

wss.on('connection', (ws) => {
  console.log('device connected');
  const session = newSession();
  const send = (message) => sendJson(ws, message);
  ws.on('close', () => stopReply(session));
  ws.on('message', (data, binary) => handleMessage(session, ws, data, binary, send));
});
