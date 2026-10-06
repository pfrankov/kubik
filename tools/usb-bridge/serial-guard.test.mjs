import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import {
  BACKOFF_MAX_MS, BACKOFF_START_MS, PortPolicy, REENUMERATION_QUIET_MS, REOPEN_GAP_MS, ROM_DOWNLOAD, SILENT_MS,
  flashToolName, openQuietly, portBusyReason,
} from './serial-guard.mjs';

// A port that records everything done to it, in order.
class FakePort extends EventEmitter {
  static made = [];
  constructor(options) {
    super();
    this.options = options; this.calls = []; this.failSet = false;
    FakePort.made.push(this);
  }
  open(callback) { this.calls.push('open'); callback(null); }
  set(lines, callback) { this.calls.push({ set: lines }); callback(this.failSet ? new Error('cannot set') : null); }
  close(callback) { this.calls.push('close'); callback?.(); }
}

test('the port opens with DTR and RTS low straight away, and hangup on close is off', async () => {
  FakePort.made = [];
  const port = await openQuietly('/dev/cu.usbmodem101', FakePort);
  assert.equal(port.options.hupcl, false);
  assert.equal(port.options.autoOpen, false);
  assert.equal(port.options.rtscts, false);
  assert.deepEqual(port.calls, ['open', { set: { dtr: false, rts: false, brk: false } }]);  // nothing in between, nothing after
});

test('a port whose lines cannot be set low is closed and refused, not used', async () => {
  const original = FakePort.prototype.set;
  FakePort.prototype.set = function (lines, callback) { this.failSet = true; return original.call(this, lines, callback); };
  try {
    await assert.rejects(openQuietly('/dev/cu.usbmodem101', FakePort), /cannot set/);
    const port = FakePort.made.at(-1);
    assert.deepEqual(port.calls.map((call) => (typeof call === 'string' ? call : 'set')), ['open', 'set', 'close']);
  } finally {
    FakePort.prototype.set = original;
  }
});

test('flashing tools are told from other processes by what they run', () => {
  assert.equal(flashToolName('python3 -m esptool --chip esp32c6 --port /dev/cu.usbmodem101 write_flash'), 'esptool');
  assert.equal(flashToolName('/Users/me/.espressif/python_env/bin/python /Users/me/esp/esp-idf/components/esptool_py/esptool/esptool.py -p x'), 'esptool.py');
  assert.equal(flashToolName('python tools/flash-device.py --port x'), 'flash-device.py');
  assert.equal(flashToolName('python /opt/esp-idf/tools/idf.py -p /dev/cu.usbmodem101 app-flash'), 'idf.py');
  assert.equal(flashToolName('python /opt/esp-idf/tools/idf.py build'), null);      // building does not touch the port
  assert.equal(flashToolName('node tools/usb-bridge/bridge.mjs --port /dev/cu.usbmodem101'), null);
  assert.equal(flashToolName('vim notes-about-esptool.txt'), null);                 // only the program counts
});

test('the port is not opened while esptool runs or another process holds it', async () => {
  const exec = (list, holders) => async (command) => (command === 'pgrep' ? list : holders);
  const flashing = exec('4242 python3 -m esptool --port /dev/cu.usbmodem101 write_flash\n77 zsh -l\n', '');
  assert.equal(await portBusyReason('/dev/cu.usbmodem101', flashing, 1), 'esptool (pid 4242)');
  assert.equal(await portBusyReason('/dev/cu.usbmodem101', exec('', '5150\n'), 1), 'held by pid 5150');
  assert.equal(await portBusyReason('/dev/cu.usbmodem101', exec('', '1\n'), 1), null);  // (the bridge's own descriptor does not count)
  assert.equal(await portBusyReason('/dev/cu.usbmodem101', exec('88 vim esptool-notes.txt\n', ''), 1), null);
  assert.equal(await portBusyReason('/dev/cu.usbmodem101', exec('4242 python3 -m esptool w\n', ''), 4242), null);
});

test('after failures the attempts back off, and a success starts again', () => {
  let now = 1e6;
  const policy = new PortPolicy(() => now);
  assert.equal(policy.wait(), BACKOFF_START_MS);
  const waits = [];
  for (let i = 0; i < 8; i++) { policy.failed(); waits.push(policy.wait()); }
  assert.deepEqual(waits.slice(0, 4), [1000, 2000, 4000, 8000]);
  assert.equal(waits.at(-1), BACKOFF_MAX_MS);
  assert(waits.every((wait, i) => !i || wait >= waits[i - 1]));
  policy.opened();
  assert.equal(policy.wait(), BACKOFF_START_MS);
});

test('the port is not touched for a few seconds after it disappears', () => {
  let now = 5e5;
  const policy = new PortPolicy(() => now);
  policy.portGone();
  assert.equal(policy.wait(), REENUMERATION_QUIET_MS);
  assert(REENUMERATION_QUIET_MS >= 3000);
  now += 2000;
  assert.equal(policy.wait(), REENUMERATION_QUIET_MS - 2000);
  now += 4000;
  assert.equal(policy.wait(), BACKOFF_START_MS);  // the quiet time has passed
  policy.portGone();                              // it went away again (still re-enumerating): the quiet time starts over
  assert.equal(policy.wait(), REENUMERATION_QUIET_MS);
  for (let i = 0; i < 6; i++) policy.failed();
  assert.equal(policy.wait(), BACKOFF_MAX_MS);    // a longer backoff is not shortened by it
});

test('a silent port is reopened rarely, and never while the ROM bootloader is waiting', () => {
  let now = 1e7;
  const policy = new PortPolicy(() => now);
  const start = now;
  assert(!policy.reopenDue(now - (SILENT_MS - 1)));           // not silent long enough
  assert(policy.reopenDue(start - SILENT_MS));                // silent: reopen
  now += 1000;
  assert(!policy.reopenDue(start - SILENT_MS));               // but not again at once
  now += REOPEN_GAP_MS;
  assert(!policy.reopenDue(start - SILENT_MS));               // the gap has doubled
  now += REOPEN_GAP_MS;
  assert(policy.reopenDue(start - SILENT_MS));
  now += 4 * REOPEN_GAP_MS;
  policy.download = true;
  assert(!policy.reopenDue(start - SILENT_MS));               // a device in the download mode is left alone
  policy.dataSeen();
  assert(!policy.download);
  now += REOPEN_GAP_MS;
  assert(policy.reopenDue(start - SILENT_MS));                // and a talking device resets the backoff
});

test('the ROM download banner is recognised', () => {
  assert(ROM_DOWNLOAD.test('ESP-ROM:esp32c6-20220919\r\nrst:0x15 (USB_UART_HPSYS),boot:0x5 (DOWNLOAD(USB/UART0/SPI))\r\nwaiting for download\r\n'));
  assert(ROM_DOWNLOAD.test('boot:0x5 (DOWNLOAD(USB/UART0/SPI))'));
  assert(!ROM_DOWNLOAD.test('rst:0x1 (POWERON),boot:0x1c (SPI_FAST_FLASH_BOOT)'));
});
