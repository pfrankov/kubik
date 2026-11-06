import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';

export function playbackEndpoint(value) {
  const url = new URL(value);
  assert.equal(url.protocol, 'http:', 'Near playback must use a loopback SSH forward');
  assert.ok(['127.0.0.1', 'localhost'].includes(url.hostname), 'Near playback must be loopback');
  assert.equal(url.pathname, '/play');
  assert.ok(!url.username && !url.password && !url.search && !url.hash, 'Unexpected playback URL fields');
  return url;
}

async function playbackResult(response) {
  const reader = response.body.getReader();
  const chunks = []; let size = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      assert.ok(size <= 256, 'Near playback response exceeds bound');
      chunks.push(value);
    }
    return JSON.parse(Buffer.concat(chunks).toString('utf8'));
  } finally { await reader.cancel(); }
}

// Only a digest is sent. The Mac owns a fixed immutable WAV and cannot be
// asked to open another file or run another command. An ambiguous write is not retried.
export async function playNearSpeech(source, { run, env = process.env, platform = process.platform,
  fetchImpl = fetch, timeoutMs = 30000 } = {}) {
  const endpoint = env.KUBIK_TEST_NEAR_PLAY_URL;
  if (!endpoint) {
    assert.equal(platform, 'darwin', 'Set KUBIK_TEST_NEAR_PLAY_URL for remote acoustic capture');
    await run('afplay', [source]);
    return;
  }
  const url = playbackEndpoint(endpoint), token = env.KUBIK_TEST_NEAR_TOKEN;
  assert.match(token ?? '', /^[a-f0-9]{64}$/, 'Set the shared KUBIK_TEST_NEAR_TOKEN');
  const sha256 = createHash('sha256').update(readFileSync(source)).digest('hex');
  const response = await fetchImpl(url, { method: 'POST', redirect: 'error',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${token}` },
    body: JSON.stringify({ sha256 }), signal: AbortSignal.timeout(timeoutMs) });
  assert.equal(response.status, 200, `Near playback returned HTTP ${response.status}; not retried`);
  const body = await playbackResult(response);
  assert.equal(body.ok, true, 'Near playback did not complete');
  assert.equal(body.sha256, sha256, 'Near playback used a different WAV');
}
