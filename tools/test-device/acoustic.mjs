import assert from 'node:assert/strict';

export function inspectTurn(log, minimumInputMs = 0) {
  assert.doesNotMatch(log, /app: speech (?:stopped|lost)|app: server error|link: link: none|link: websocket error|esp_transport_write\(\) returned|microphone delivery overflow|speech buffer|reservation failed|allocation failed|alloc \d+ failed|read error|Guru Meditation|Brownout|PANIC|assert failed/,
    'Device reported a failed or truncated turn');
  const begin = [...log.matchAll(/app: speech begin gen=(\d+)/g)];
  assert.ok(begin.length <= 1, 'Unexpected second reply');
  if (!begin.length) return null;
  const played = new RegExp(`app: speech played gen=${begin[0][1]} ms=(\\d+) elapsed=(\\d+) gaps=(\\d+) \\((\\d+) ms\\).*via=(\\w+)`).exec(log);
  if (!played) return null;
  assert.ok(+played[1] > 0, 'Empty reply');
  assert.equal(+played[3], 0, 'Playback underruns');
  assert.equal(+played[4], 0, 'Playback underrun duration');
  assert.equal(played[5], 'wifi', 'Reply changed transport');
  const input = /voice: input frames=(\d+) pcm=(\d+)ms elapsed=(\d+)ms send=(\d+)ms max=(\d+)ms over40=(\d+) dropped=(\d+)ms/.exec(log);
  assert.ok(input, 'Missing actual input summary');
  assert.ok(+input[1] > 0, 'No microphone frames');
  assert.ok(+input[2] >= minimumInputMs, 'Input ended before the spoken question completed');
  assert.equal(+input[7], 0, 'Microphone RX queue lost audio');
  return { generation: +begin[0][1], speechMs: +played[1], elapsedMs: +played[2], inputMs: +input[2], inputWallMs: +input[3] };
}

export function inspectCapture(wav, recorderLog) {
  assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
  assert.equal(wav.toString('ascii', 8, 12), 'WAVE');
  let rate = 0, bytes = 0;
  for (let offset = 12; offset + 8 <= wav.length;) {
    const kind = wav.toString('ascii', offset, offset + 4), length = wav.readUInt32LE(offset + 4);
    const at = offset + 8;
    assert.ok(at + length <= wav.length, 'Truncated WAV chunk');
    if (kind === 'fmt ') {
      assert.ok(length >= 16);
      assert.equal(wav.readUInt16LE(at), 1, 'Expected PCM');
      assert.equal(wav.readUInt16LE(at + 2), 1);
      assert.equal(wav.readUInt32LE(at + 4), 48000);
      assert.equal(wav.readUInt16LE(at + 14), 16);
      rate = wav.readUInt32LE(at + 8);
    }
    if (kind === 'data') bytes += length;
    offset = at + length + (length & 1);
  }
  const timing = /audio=([\d.]+) wall=([\d.]+)/.exec(recorderLog);
  assert.ok(timing && rate && bytes, 'Missing recording or duration');
  const duration = bytes / rate, wall = +timing[2];
  assert.ok(Math.abs(duration - wall) < .1, 'Capture has missing time');
  assert.ok(Math.abs(duration - +timing[1]) < .1, 'Recorder/WAV disagree');
  return { seconds: duration, wallSeconds: wall };
}
