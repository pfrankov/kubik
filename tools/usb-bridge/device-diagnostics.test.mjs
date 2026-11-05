import assert from 'node:assert/strict';
import test from 'node:test';
import { DeviceDiagnostics } from './device-diagnostics.mjs';

test('fault snapshot retains every prior fault class without retaining log contents', () => {
  const d = new DeviceDiagnostics(), original = d.snapshot();
  d.consume('main: display 30 fps'); assert.deepEqual(d.snapshot(), original);
  const faults=['speech buffer overflow','reservation failed','alloc 968 failed','allocation failed bytes=1682',
    'read error','PANIC','assert failed'];
  for(const text of faults) d.consume(text);
  d.consume('ESP-ROM:esp32c6'); d.disconnects++;
  assert.deepEqual(d.snapshot(), {...original,faults:faults.length,boots:1,disconnects:1});
  assert.notEqual(new DeviceDiagnostics().snapshot().instance,original.instance);
  assert.equal(JSON.stringify(d.snapshot()).includes('overflow'),false);
});
test('counts supported chip ROM boots and ignores unrelated chips', () => {
  const d = new DeviceDiagnostics();
  d.consume('ESP-ROM:esp32s3-20210327');
  d.consume('ESP-ROM:esp32c6-20220919');
  d.consume('ESP-ROM:esp32c3-20200918');
  assert.equal(d.snapshot().boots, 2);
});

test('actual Xtensa panic is a fault; normal motion logs are not', () => {
  const d = new DeviceDiagnostics();
  d.consume("Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.");
  assert.equal(d.snapshot().faults, 1);
  d.consume('input: gravity screen -0.04,0.98,-0.02 pull on the screen -0.04,0.98');
  d.consume('main: mode 1 on screen');
  assert.equal(d.snapshot().faults, 1);
});
