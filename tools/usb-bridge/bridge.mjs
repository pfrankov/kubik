#!/usr/bin/env node
// Kubik USB bridge: carries the device protocol over the USB Serial/JTAG port
// and relays it to the kubik channel plugin's WebSocket, so the device works
// on a desk without Wi-Fi (and during development).
//
//   node bridge.mjs [--port /dev/cu.usbmodem101] [--server kubik://192.168.1.20 | wss://host/kubik/v1] [--quiet]
//   node bridge.mjs config '{"cmd":"info"}'          # send a config command and print the reply
//   node bridge.mjs config '{"cmd":"set","url":"wss://your-host/kubik/v1"}'
//
// Framing (docs/protocol.md): A5 5A type len16le payload crc8(payload), CRC-8 poly 0x07.
import { parseAudioFrame } from '../../openclaw-kubik/src/protocol.js';
import { SerialPort } from 'serialport';
import fs from 'node:fs';
import http from 'node:http';
import { createSetupHandler } from './setup-page.mjs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { F } from './bridge-protocol.mjs';
import { UsbRoute } from './usb-route.mjs';
import { PortPolicy, ROM_DOWNLOAD, openQuietly, portBusyReason } from './serial-guard.mjs';
import { SerialWriter } from './serial-writer.mjs';

export { F, UsbRoute };

export function crc8(buf, crc = 0) {
  for (const b of buf) {
    crc ^= b;
    for (let i = 0; i < 8; i++) crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
  }
  return crc;
}

export function frame(type, payload) {
  const p = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  const out = Buffer.alloc(6 + p.length);
  out[0] = 0xa5;
  out[1] = 0x5a;
  out[2] = type;
  out.writeUInt16LE(p.length, 3);
  p.copy(out, 5);
  out[5 + p.length] = crc8(p);
  return out;
}

// Incremental parser: emits frames and passes through plain text (boot log, panics).
// A frame cut short on the device (a timed-out USB write) leaves a header whose length swallows what follows;
// lengths no frame can have, and a frame still incomplete after `staleMs`, are treated as noise.
export const MAX_FRAME_PAYLOAD = 8192;
export class Deframer {
  constructor(onFrame, onText, { staleMs = 500, now = () => Date.now() } = {}) {
    this.buf = Buffer.alloc(0);
    this.onFrame = onFrame;
    this.onText = onText;
    this.staleMs = staleMs;
    this.now = now;
    this.waitingSince = null;
  }
  push(chunk) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    for (;;) {
      const start = this.buf.indexOf(0xa5);
      if (start < 0) {
        this.#text(this.buf);
        this.buf = Buffer.alloc(0);
        return;
      }
      if (start > 0) {
        this.#text(this.buf.subarray(0, start));
        this.buf = this.buf.subarray(start);
      }
      if (this.buf.length < 2) return;
      if (this.buf[1] !== 0x5a) {
        this.#text(this.buf.subarray(0, 1));
        this.buf = this.buf.subarray(1);
        continue;
      }
      if (this.buf.length < 5) return this.#wait();
      const len = this.buf.readUInt16LE(3);
      if (len > MAX_FRAME_PAYLOAD || (this.buf.length < 6 + len && this.#wait() === false)) {
        this.waitingSince = null;
        this.#text(this.buf.subarray(0, 1));
        this.buf = this.buf.subarray(1);
        continue;
      }
      if (this.buf.length < 6 + len) return;
      this.waitingSince = null;
      const payload = this.buf.subarray(5, 5 + len);
      const crc = this.buf[5 + len];
      if (crc8(payload) === crc) {
        this.onFrame(this.buf[2], Buffer.from(payload));
        this.buf = this.buf.subarray(6 + len);
      } else {
        this.#text(this.buf.subarray(0, 1));
        this.buf = this.buf.subarray(1);
      }
    }
  }
  /** Keeps waiting for the rest of a frame; false once it has been incomplete for too long. */
  #wait() {
    const now = this.now();
    if (this.waitingSince === null) this.waitingSince = now;
    return now - this.waitingSince < this.staleMs ? undefined : false;
  }
  #text(b) {
    if (b.length) this.onText(b.toString('utf8'));
  }
}

async function findPort() {
  const ports = await SerialPort.list();
  const esp = ports.find((p) => (p.vendorId || '').toLowerCase() === '303a');
  if (esp) return esp.path.replace('/dev/tty.', '/dev/cu.');
  const any = ports.find((p) => /usbmodem/.test(p.path));
  return any?.path.replace('/dev/tty.', '/dev/cu.');
}

function parseArgs(argv) {
  const o = { server: process.env.KUBIK_SERVER || null, quiet: false, cmd: null };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port') o.port = argv[++i];
    else if (a === '--server') o.server = argv[++i];
    else if (a === '--quiet') o.quiet = true;
    else if (a === '--no-usb-route') o.usbRoute = false;
    else if (a === '--dump') o.dump = argv[++i];
    else if (a === '--control') o.control = Number(argv[++i]);
    else if (a === 'config') o.cmd = argv[++i] || '{"cmd":"info"}';
    else if (a === '-h' || a === '--help') o.help = true;
  }
  return o;
}

const ts = () => new Date().toISOString().slice(11, 23);

async function runConfig(opts) {
  const path = opts.port || (await findPort());
  if (!path) throw new Error('Kubik not found on USB');
  const busy = await portBusyReason(path);
  if (busy) throw new Error(`The USB port is busy: ${busy}`);
  const port = await openQuietly(path);
  let writer;
  const done = new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('no reply from device')), 4000);
    writer = new SerialWriter(port, reason => { clearTimeout(t); reject(new Error(reason)); });
    const d = new Deframer(
      (type, p) => {
        if (type === F.CONFIG_REPLY) {
          clearTimeout(t);
          resolve(p.toString('utf8'));
        }
      },
      () => {},
    );
    port.on('data', (c) => d.push(c));
  });
  writer.send(frame(F.CONFIG, Buffer.from(opts.cmd)));
  try {
    console.log(await done);
  } finally {
    writer.close();
    port.close();
  }
}

async function runBridge(opts) {
  let port = null;
  let writer = null;
  const policy = new PortPolicy();
  let lastBusy = null;
  const stats = { up: 0, down: 0, upAudio: 0, downAudio: 0 };
  let textLine = '';
  let turnPcm = null, turnBytes = 0, dumpTurn = 0;
  const configWaiters = [];
  const log = (...a) => console.log(ts(), ...a);
  const route = new UsbRoute({ server: opts.server, enabled: opts.usbRoute !== false, log,
    send(type, data) {
      if (type === F.AUDIO) stats.downAudio += Math.max(0, data.length - 4);
      toDevice(type, data);
    },
  });

  // Local control endpoint: POST http://127.0.0.1:18791/config with a config JSON body
  // (same commands as `bridge.mjs config`), usable while the bridge owns the port.
  if (opts.control !== 0) {
    http
      .createServer(createSetupHandler({
        port: opts.control ?? 18791,
        config: (command) => new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            const i = configWaiters.indexOf(done);
            if (i >= 0) configWaiters.splice(i, 1);
            reject(new Error('Device did not reply. Check its USB connection.'));
          }, 3000);
          const done = (reply) => { clearTimeout(timer); resolve(reply); };
          configWaiters.push(done);
          toDevice(F.CONFIG, Buffer.from(JSON.stringify(command)));
        }),
        route: async ({ enabled }) => {
          route.setEnabled(enabled);
          return { ok: true, enabled: route.enabled };
        },
      }))
      .listen(opts.control ?? 18791, '127.0.0.1');
  }

  // --dump DIR: save every push-to-talk utterance as a WAV (debugging the microphone).
  function saveTurn(turn) {
    if (!opts.dump || !turnPcm) return;
    const pcm = Buffer.concat(turnPcm);
    turnPcm = null;
    fs.mkdirSync(opts.dump, { recursive: true });
    const h = Buffer.alloc(44);
    h.write('RIFF', 0);
    h.writeUInt32LE(36 + pcm.length, 4);
    h.write('WAVEfmt ', 8);
    h.writeUInt32LE(16, 16);
    h.writeUInt16LE(1, 20);
    h.writeUInt16LE(1, 22);
    h.writeUInt32LE(24000, 24);
    h.writeUInt32LE(48000, 28);
    h.writeUInt16LE(2, 32);
    h.writeUInt16LE(16, 34);
    h.write('data', 36);
    h.writeUInt32LE(pcm.length, 40);
    const file = path.join(opts.dump, `turn-${Date.now()}-${turn}.wav`);
    fs.writeFileSync(file, Buffer.concat([h, pcm]));
    log(`saved ${file} (${(pcm.length / 48000).toFixed(2)} s)`);
  }

  function toDevice(type, payload) {
    if (port?.isOpen) writer?.send(frame(type, payload));
  }

  function handleFrame(type, payload) {
    policy.dataSeen();
    if (type === F.LOG) handleLogFrame(payload);
    else if (type === F.JSON) handleJsonFrame(payload);
    else if (type === F.AUDIO) handleAudioFrame(payload);
    else if (type === F.CONFIG_REPLY) handleConfigReply(payload);
  }

  function handleLogFrame(payload) {
    if (!opts.quiet) log('[dev]', payload.toString('utf8').replace(/\x1b\[[0-9;]*m/g, ''));
  }

  function handleJsonFrame(payload) {
    const routed = route.deviceFrame(F.JSON, payload);
    if (!routed) return;
    const text = routed.toString('utf8');
    let msg = null;
    try { msg = JSON.parse(text); } catch {}
    if (opts.dump && msg?.t === 'ptt' && msg.on) { turnPcm = []; turnBytes = 0; dumpTurn = msg.turn; }
    if (msg?.t === 'ptt' && !msg.on) saveTurn(msg.turn);
    if (!opts.quiet && msg?.t !== 'ping') log('dev→srv', text);
  }

  function handleAudioFrame(payload) {
    const routed = route.deviceFrame(F.AUDIO, payload);
    if (!routed) return;
    stats.upAudio += routed.length;
    if (!turnPcm) return;
    try {
      const audio = parseAudioFrame(routed);
      if (audio.turn !== dumpTurn) return;
      turnBytes += audio.pcm.length;
      if (turnBytes <= 61 * 48000) turnPcm.push(audio.pcm);
      else { turnPcm = null; log('microphone dump exceeded 61 seconds'); }
    } catch { turnPcm = null; log('invalid microphone dump frame'); }
  }

  function handleConfigReply(payload) {
    const reply = payload.toString('utf8');
    const waiter = configWaiters.shift();
    if (waiter) waiter(reply);
    else log('config reply', reply);
  }

  function handleText(text) {
    if (ROM_DOWNLOAD.test(text) && !policy.download) {
      policy.download = true;
      log('device is in the ROM download mode (it was reset with BOOT low): it will not run until USB is replugged');
    }
    textLine = (textLine + text).slice(-8192);
    let newline;
    while ((newline = textLine.indexOf('\n')) >= 0) {
      const line = textLine.slice(0, newline).replace(/\r$/, '').replace(/\x1b\[[0-9;]*m/g, '');
      textLine = textLine.slice(newline + 1);
      if (line.trim() && !opts.quiet) log('[dev]', line);
    }
  }

  const deframer = new Deframer(handleFrame, handleText);

  // The port is opened only when nothing else has it, and not until the quiet time after it disappeared has passed
  // (serial-guard.mjs: the chip re-enumerates after a reset, and a line change then can leave it stuck).
  const retry = () => setTimeout(attach, policy.wait());

  async function attach() {
    const path = opts.port || (await findPort());
    if (!path || !fs.existsSync(path)) {
      policy.portGone();
      retry();
      return;
    }
    const busy = await portBusyReason(path);
    if (busy) {
      if (busy !== lastBusy) log(`USB port busy (${busy}), waiting`);
      lastBusy = busy;
      policy.failed();
      retry();
      return;
    }
    lastBusy = null;
    try {
      port = await openQuietly(path);
    } catch (e) {
      policy.failed();
      retry();
      return;
    }
    policy.opened();
    log(`device port ${path}`);
    const opened = port;
    writer = new SerialWriter(opened, reason => {
      log(reason);
      route.detach();
      if (opened.isOpen) opened.close(() => {});
    });
    lastData = Date.now();
    port.on('data', (c) => { lastData = Date.now(); deframer.push(c); });
    route.attach();
    port.on('close', () => {
      log('device disconnected');
      writer?.close(); writer = null;
      port = null;
      route.detach();
      deframer.buf = Buffer.alloc(0); textLine = ''; turnPcm = null;
      policy.portGone();
      retry();
    });
    port.on('error', (e) => log('port error:', e.message));
  }

  setInterval(() => route.announce(), 1000);
  // macOS sometimes stops delivering data from the device's USB serial after a burst; the device logs at least
  // every 5 s, so a long silence means a stuck port: reopening it brings the stream back. (Every open raises DTR and
  // RTS for a moment, so this is rare, backs off, and waits out a device that is in the ROM download mode.)
  let lastData = 0;
  setInterval(() => {
    if (port?.isOpen && policy.reopenDue(lastData)) {
      log('device silent for 30 s, reopening the port');
      port.close(() => {});
    }
  }, 1000);
  setInterval(() => {
    if (stats.upAudio || stats.downAudio)
      log(`audio: mic ${(stats.upAudio / 1024).toFixed(0)} KiB up, speech ${(stats.downAudio / 1024).toFixed(0)} KiB down`);
    stats.upAudio = stats.downAudio = 0;
  }, 10000);
  await attach();
  log(opts.server ? 'using explicit server override' : 'using the server configured on the device');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
const opts = parseArgs(process.argv.slice(2));
if (opts.help) {
  console.log('usage: bridge.mjs [--port PATH] [--server kubik://HOST[:PORT]|wss://URL] [--quiet] [--dump DIR] [--no-usb-route] | config JSON');
} else if (opts.cmd) {
  runConfig(opts).catch((e) => {
    console.error(e.message);
    process.exit(1);
  });
} else {
  runBridge(opts);
}

}
