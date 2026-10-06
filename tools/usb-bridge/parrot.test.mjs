import { ImaEncoder, KIND_MIC } from '../../openclaw-kubik/src/protocol.js';
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { generateKeyPairSync, randomBytes, sign } from 'node:crypto';
import net from 'node:net';
import WebSocket from 'ws';
import { authMessage, KIND_SPEECH_IMA } from '../../openclaw-kubik/src/protocol.js';

async function startParrot(t) {
  const port = await new Promise(resolve => {
    const socket = net.createServer(); socket.listen(0, '127.0.0.1', () => {
      const selected = socket.address().port; socket.close(() => resolve(selected));
    });
  });
  const child = spawn(process.execPath, [new URL('./parrot-server.mjs', import.meta.url).pathname, String(port)],
    { stdio: ['ignore', 'pipe', 'pipe'] });
  t.after(() => child.kill());
  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(Error('parrot did not start')), 5000);
    child.stdout.on('data', data => {
      if (data.toString().includes('parrot on')) { clearTimeout(timeout); resolve(); }
    });
    child.once('exit', code => { clearTimeout(timeout); reject(Error(`parrot exited ${code}`)); });
  });
  return port;
}

const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
const jwk = publicKey.export({ format: 'jwk' });
const key = Buffer.concat([Buffer.from([4]), Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')]).toString('base64');
const hello = { t: 'hello', v: 5, device: 'kubik-test', key, fw: '0.6.1' };
const connect = port => new Promise(resolve => {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/kubik/v1`);
  ws.once('open', () => resolve(ws));
});
const exchange = ws => new Promise(resolve => ws.once('message', data => resolve(JSON.parse(data.toString()))));

test('parrot requires a valid v5 device signature before welcome', async t => {
  const port = await startParrot(t);
  const good = await connect(port); t.after(() => good.terminate());
  good.send(JSON.stringify(hello));
  const challenge = await exchange(good); assert.equal(challenge.t, 'challenge');
  const signature = sign('sha256', authMessage({ nonce: challenge.nonce, device: hello.device, key, bind: 'none' }), privateKey);
  good.send(JSON.stringify({ t: 'auth', sig: signature.toString('base64') }));
  assert.equal((await exchange(good)).t, 'welcome');
  const bad = await connect(port); t.after(() => bad.terminate());
  bad.send(JSON.stringify(hello));
  await exchange(bad);
  const closed = new Promise(resolve => bad.once('close', resolve));
  bad.send(JSON.stringify({ t: 'auth', sig: randomBytes(71).toString('base64') }));
  assert.equal(await closed, 4001);
});

test('parrot speaks the utterance back as IMA ADPCM frames', async t => {
  const port = await startParrot(t);
  const ws = await connect(port); t.after(() => ws.terminate());
  ws.send(JSON.stringify(hello));
  const challenge = await exchange(ws);
  const signature = sign('sha256', authMessage({ nonce: challenge.nonce, device: hello.device, key, bind: 'none' }), privateKey);
  ws.send(JSON.stringify({ t: 'auth', sig: signature.toString('base64') }));
  await exchange(ws);
  const speech = [];
  const ended = new Promise(resolve => ws.on('message', (data, binary) => {
    if (binary) speech.push(data);
    else if (JSON.parse(data.toString()).t === 'speak_end') resolve();
  }));
  ws.send(JSON.stringify({ t: 'ptt', on: true, turn: 1 }));
  const encoder = new ImaEncoder();
  for (let i = 0; i < 25; i++) ws.send(Buffer.concat([Buffer.from([KIND_MIC, 1]), encoder.encode(Buffer.alloc(1920, i))]));  // 1 s of PCM
  ws.send(JSON.stringify({ t: 'ptt', on: false, turn: 1, ms: 1000 }));
  await ended;
  assert.ok(speech.length >= 10 && speech.every(frame => frame[0] === KIND_SPEECH_IMA && frame[1] === 1));
  assert.equal(speech[0].length, 2 + 3 + 4800 / 4);  // tag, 3 bytes of encoder state, a quarter of 100 ms of PCM
});
