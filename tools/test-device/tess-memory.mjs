import assert from 'node:assert/strict';

// A bridge restart must never turn a lost fault history into a passing result.
export class TessContinuity {
  constructor(info, bridge, now) {
    assert.ok(Number.isSafeInteger(info.uptime_ms) && info.uptime_ms >= 0, 'device uptime required');
    assert.ok(Number.isSafeInteger(info.heap_failures) && info.heap_failures >= 0, 'allocation diagnostics required');
    this.minimumStack = info.chip === 'esp32s3' ? 1024 : 1;
    this.stack(info);
    assert.equal(bridge.ok, true);
    assert.ok(typeof bridge.instance === 'string' && bridge.instance.length, 'bridge identity required');
    for (const key of ['faults', 'boots', 'disconnects'])
      assert.ok(Number.isSafeInteger(bridge[key]) && bridge[key] >= 0, `bridge ${key} required`);
    assert.equal(bridge.faults, 0, 'start with a fresh bridge fault history');
    this.first = info.uptime_ms; this.last = info.uptime_ms; this.start = now;
    this.bridge = {...bridge}; this.heapFailures = info.heap_failures;
  }
  stack(info) {
    assert.ok(Number.isSafeInteger(info.app_stack_free) && info.app_stack_free >= this.minimumStack,
      `app-task stack reserve too small: ${info.app_stack_free}`);
  }
  check(info, bridge, now) {
    this.stack(info);
    assert.ok(Number.isSafeInteger(info.uptime_ms) && info.uptime_ms >= this.last, 'device uptime reset');
    assert.ok(info.uptime_ms >= this.first + now - this.start - 1000, 'device uptime did not advance continuously');
    assert.equal(info.heap_failures, this.heapFailures, 'device allocation failure');
    assert.equal(bridge.instance, this.bridge.instance, 'bridge restarted; fault history lost');
    for (const key of ['faults', 'boots', 'disconnects'])
      assert.equal(bridge[key], this.bridge[key], `device ${key} changed`);
    this.last = info.uptime_ms;
  }
}

export async function repeatScatter({ tap, game, check, wait, pause, prepare = async () => {}, cycles = 4 }) {
  assert.ok(Number.isInteger(cycles) && cycles >= 1 && cycles <= 8, 'bounded scatter cycle count required');
  for (let cycle = 0; cycle < cycles; cycle++) {
    await prepare(cycle);
    for (let n = 0; n < 6; n++) { await tap(); await check(); }
    await wait(game, state => state.preview && state.burst > 0, `cycle ${cycle + 1}: real taps scatter Tess`, 3000);
    await check();
    // No finger is left down: the actual burst returns under its own clock.
    await wait(game, state => state.preview && state.burst === 0 && state.game === 0,
      `cycle ${cycle + 1}: scattered dots return to the form`, 14000);
    await pause(500);
    await check();
  }
}
