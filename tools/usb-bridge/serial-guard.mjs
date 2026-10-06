// How the bridge touches the device's USB Serial/JTAG port without resetting the device.
//
// DTR and RTS of that port are wired to the chip's boot strap and reset: opening the port raises both lines on
// macOS, and a line change at the wrong moment (during a reset, or while the chip is still enumerating after one)
// leaves the chip in the ROM download mode or stuck before the app starts, until USB is replugged. So the port is
// opened with the lines set low at once, is left alone while another tool (esptool, idf.py) has it, and is not
// opened again until a few seconds after it disappeared (the re-enumeration after a reset).
import { execFile } from 'node:child_process';
import { SerialPort } from 'serialport';

export const REENUMERATION_QUIET_MS = 5000;  // after the port went away: the chip is resetting and enumerating again
export const BACKOFF_START_MS = 1000;        // between attempts while it cannot be opened, doubling up to the maximum
export const BACKOFF_MAX_MS = 15000;
export const SILENT_MS = 30000;              // no data for this long: the port is probably stuck (the device logs every 5 s)
export const REOPEN_GAP_MS = 60000;          // a stuck port is reopened at most this often, doubling up to the maximum
export const REOPEN_GAP_MAX_MS = 600000;
export const ROM_DOWNLOAD = /waiting for download|boot:0x[0-9a-f]+ \(DOWNLOAD/;  // what the ROM bootloader says when it holds still

// Programs that flash, monitor or reset the device and must not be disturbed: told apart in a process list.
const TOOL_WORD = /(^|\/)(esptool(\.py)?|flash-device\.py|openocd)$/;
const IDF_ACTION = /flash|monitor|erase/;
const basename = (word) => word.slice(word.lastIndexOf('/') + 1);
/** The name of the flashing tool a command line runs ("esptool", "idf.py"), or null. */
export function flashToolName(command) {
  const words = command.trim().split(/\s+/);
  const tool = words.slice(0, 4).find((word) => TOOL_WORD.test(word));
  if (tool) return basename(tool);
  return words.slice(0, 3).some((word) => basename(word) === 'idf.py') && words.some((word) => IDF_ACTION.test(word)) ? 'idf.py' : null;
}

const run = (command, args) => new Promise((resolve) => {
  execFile(command, args, { timeout: 3000 }, (error, stdout) => resolve(error && !stdout ? '' : String(stdout)));
});

/** Why the port must not be opened now ("esptool (pid 123)"), or null when nothing else has it. */
export async function portBusyReason(path, exec = run, self = process.pid) {
  const listed = (await exec('pgrep', ['-fl', 'esptool|idf\\.py|flash-device|openocd'])).split('\n')
    .map((line) => line.trim().match(/^(\d+)\s+(.*)$/)).filter((match) => match && Number(match[1]) !== self);
  for (const [, pid, command] of listed) {
    const tool = flashToolName(command);
    if (tool) return `${tool} (pid ${pid})`;
  }
  const holders = (await exec('lsof', ['-t', path])).split('\n').map(Number).filter((pid) => pid && pid !== self);
  return holders.length ? `held by pid ${holders[0]}` : null;
}

/** Opens the port with DTR and RTS low, and never leaves it open with the lines in any other state. */
export async function openQuietly(path, PortClass = SerialPort) {
  const port = new PortClass({ path, baudRate: 921600, hupcl: false, rtscts: false, autoOpen: false });
  await new Promise((resolve, reject) => port.open((error) => (error ? reject(error) : resolve())));
  try {
    await new Promise((resolve, reject) => port.set({ dtr: false, rts: false, brk: false }, (error) => (error ? reject(error) : resolve())));
  } catch (error) {
    port.close(() => {});
    throw error;
  }
  return port;
}

/** When the bridge may try the port again, and when to give up on one that has gone quiet. */
export class PortPolicy {
  constructor(now = Date.now) {
    Object.assign(this, { now, goneAt: -Infinity, failures: 0, reopenedAt: -Infinity, reopenGap: REOPEN_GAP_MS, download: false });
  }
  /** The port disappeared, or is not there: the chip is (re)starting, keep away for a while. */
  portGone() { this.goneAt = this.now(); }
  opened() { this.failures = 0; }
  failed() { this.failures++; }
  dataSeen() { this.reopenGap = REOPEN_GAP_MS; this.download = false; }
  /** ms to wait before the next attempt: the backoff of the failures so far, and the rest of the quiet time. */
  wait() {
    const backoff = Math.min(BACKOFF_MAX_MS, BACKOFF_START_MS * 2 ** Math.max(0, this.failures - 1));
    return Math.max(backoff, this.goneAt + REENUMERATION_QUIET_MS - this.now());
  }
  /** Silence long enough to reopen a stuck port, not too often, and never while the ROM bootloader is waiting. */
  reopenDue(lastData) {
    const now = this.now();
    if (this.download || now - lastData < SILENT_MS || now - this.reopenedAt < this.reopenGap) return false;
    this.reopenedAt = now;
    this.reopenGap = Math.min(REOPEN_GAP_MAX_MS, this.reopenGap * 2);
    return true;
  }
}
