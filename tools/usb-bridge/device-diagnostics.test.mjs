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
