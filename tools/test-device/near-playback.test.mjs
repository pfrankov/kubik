import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { once } from 'node:events';
import test from 'node:test';
import { playbackEndpoint, playNearSpeech } from './near-playback.mjs';

test('remote playback accepts only an explicit loopback URL', () => {
  assert.equal(playbackEndpoint('http://127.0.0.1:18998/play').port, '18998');
  for (const url of ['https://127.0.0.1/play', 'http://example.com/play', 'http://localhost/other',
    'http://user@localhost/play', 'http://localhost/play?path=x', 'http://localhost/play#x']) {
    assert.throws(() => playbackEndpoint(url));
  }
});

test('remote playback waits for HTTP completion with token and exact source digest', async t => {
  const directory = mkdtempSync(join(tmpdir(), 'kubik-near-client-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  const source = join(directory, 'near.wav'), data = Buffer.from('fixed fixture bytes');
  writeFileSync(source, data);
  const sha256 = createHash('sha256').update(data).digest('hex'), token = 'a'.repeat(64);
  let requests = 0;
  const server = createServer(async (request, response) => {
    requests++;
    assert.equal(request.method, 'POST');
    assert.equal(request.url, '/play');
    assert.equal(request.headers.authorization, `Bearer ${token}`);
    let body = '';
    for await (const chunk of request) body += chunk;
    assert.deepEqual(JSON.parse(body), { sha256 });
    response.setHeader('content-type', 'application/json');
    response.end(JSON.stringify({ ok: true, sha256 }));
  });
  server.listen(0, '127.0.0.1'); await once(server, 'listening');
  t.after(() => new Promise(resolve => server.close(resolve)));
  const env = { KUBIK_TEST_NEAR_PLAY_URL: `http://127.0.0.1:${server.address().port}/play`, KUBIK_TEST_NEAR_TOKEN: token };
  await playNearSpeech(source, { env, platform: 'linux', run: () => assert.fail('local playback forbidden') });
  assert.equal(requests, 1);
  await assert.rejects(playNearSpeech(source, { env: { ...env, KUBIK_TEST_NEAR_TOKEN: '' } }));
  assert.equal(requests, 1);
});

test('local route requires macOS and has no remote fallback', async () => {
  const calls = [];
  await playNearSpeech('/fixed.wav', { env: {}, platform: 'darwin', run: async (...args) => calls.push(args) });
  assert.deepEqual(calls, [['afplay', ['/fixed.wav']]]);
  await assert.rejects(playNearSpeech('/fixed.wav', { env: {}, platform: 'linux' }));
});

test('remote response is bounded and cannot claim a different WAV', async t => {
  const directory = mkdtempSync(join(tmpdir(), 'kubik-near-bounds-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  const source = join(directory, 'near.wav'); writeFileSync(source, 'fixture');
  const env = { KUBIK_TEST_NEAR_PLAY_URL: 'http://127.0.0.1/play', KUBIK_TEST_NEAR_TOKEN: 'a'.repeat(64) };
  for (const body of ['x'.repeat(257), JSON.stringify({ ok: true, sha256: '0'.repeat(64) })]) {
    let calls = 0;
    await assert.rejects(playNearSpeech(source, { env, fetchImpl: async () => {
      calls++; return new Response(body, { status: 200 });
    } }));
    assert.equal(calls, 1);
  }
});

test('remote timeout is bounded and never retries ambiguous playback', async t => {
  const directory = mkdtempSync(join(tmpdir(), 'kubik-near-timeout-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  const source = join(directory, 'near.wav'); writeFileSync(source, 'fixture');
  let requests = 0;
  const server = createServer(() => { requests++; });
  server.listen(0, '127.0.0.1'); await once(server, 'listening');
  t.after(() => { server.closeAllConnections(); server.close(); });
  const env = { KUBIK_TEST_NEAR_PLAY_URL: `http://127.0.0.1:${server.address().port}/play`, KUBIK_TEST_NEAR_TOKEN: 'a'.repeat(64) };
  await assert.rejects(playNearSpeech(source, { env, timeoutMs: 80 }), { name: 'TimeoutError' });
  assert.equal(requests, 1);
});
