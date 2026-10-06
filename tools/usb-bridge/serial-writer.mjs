// USB Serial/JTAG ignores the UART baud rate. ESP-IDF drops packets when its
// 4096-byte RX ring fills; a WebSocket burst must not be one unbounded USB write.
export const SERIAL_CHUNK_BYTES = 512;
export const SERIAL_PAUSE_MS = 5;
export const SERIAL_QUEUE_BYTES = 32768;

export class SerialWriter {
  constructor(port, fail, { setTimer = setTimeout, clearTimer = clearTimeout } = {}) {
    Object.assign(this, { port, fail, setTimer, clearTimer });
    this.queue = []; this.bytes = 0; this.offset = 0;
    this.busy = false; this.closed = false; this.timer = null;
  }
  send(data) {
    if (this.closed) return false;
    if (!data.length) return true;
    if (this.bytes + data.length > SERIAL_QUEUE_BYTES) return this.abort('USB output queue full');
    this.queue.push(data); this.bytes += data.length;
    if (!this.busy) this.write();
    return true;
  }
  close() {
    this.closed = true; this.clearTimer(this.timer); this.timer = null;
    this.queue = []; this.bytes = 0;
  }
  abort(reason) {
    if (!this.closed) { this.close(); this.fail(reason); }
    return false;
  }
  write() {
    if (this.closed) return;
    this.busy = this.queue.length > 0;
    if (!this.busy) return;
    const chunk = this.queue[0].subarray(this.offset, this.offset + SERIAL_CHUNK_BYTES);
    this.timer = this.setTimer(() => this.abort('USB write timed out'), 1000);
    try {
      this.port.write(chunk, error => {
        if (this.closed) return;
        if (error) { this.abort('USB write failed'); return; }
        this.drain(chunk.length);
      });
    } catch { this.abort('USB write failed'); }
  }
  drain(length) {
    try {
      this.port.drain(error => {
        if (this.closed) return;
        if (error) { this.abort('USB drain failed'); return; }
        this.clearTimer(this.timer);
        this.bytes -= length; this.offset += length;
        if (this.offset === this.queue[0].length) { this.queue.shift(); this.offset = 0; }
        this.timer = this.setTimer(() => this.write(), SERIAL_PAUSE_MS);
      });
    } catch { this.abort('USB drain failed'); }
  }
}
