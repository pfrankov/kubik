import { PrinterDeviceSession } from './printer-session.js';
import { screenEmojiInText } from './screen-emoji-contract.js';
export { ColorEmojiAtlas } from './screen-emoji-atlas.js';

/** At most one current page and one authorization/read job; newer pages replace pending work. */
export class ScreenEmojiChannel {
  #open = false;
  #busy = false;
  #page = null;
  #source = new Set();
  #tokens = 4;
  #time;

  constructor(session, atlas, { now = () => performance.now() } = {}) {
    Object.assign(this, { session, atlas, now }); this.#time = now();
  }
  start() { this.#open = true; }
  close() { this.#open = false; this.#page = null; this.#source.clear(); }
  card(text) { this.#page = null; this.#source = screenEmojiInText(text); }
  #current() { const s = this.session; return this.#open && !s.closed && s.current() === s && s.ws.readyState === 1 && (s.ws.bufferedAmount ?? 0) <= 64 * 1024; }

  #admit() {
    const time = this.now();
    this.#tokens = Math.min(4, this.#tokens + Math.max(0, time - this.#time) / 500); this.#time = time;
    if (this.#tokens < 1) return false;
    this.#tokens--; return true;
  }

  #reply(page, entries) {
    for (const seq of page.seq) {
      if (!this.#current() || this.#page !== page) return;
      const glyph = this.#source.has(seq) ? entries?.get(seq) : null;
      this.session.send({ t: 'emoji', id: page.id, seq, data: glyph ? glyph.toString('base64') : null });
    }
  }

  handle(message) {
    if (!this.#current()) return;
    const page = this.#page = { id: message.id, seq: message.seq };
    if (!this.#admit()) { this.#reply(page, null); this.#page = null; return; }
    if (this.#busy) return;
    this.#busy = true;
    return this.#serve().catch(() => {}).finally(() => { this.#busy = false; this.#page = null; });
  }

  async #serve() {
    if (!await this.session.authorize() || !this.#current() || !this.#page) return;
    const page = this.#page;
    const entries = page.seq.some(seq => this.#source.has(seq)) ? await this.atlas?.load() : null;
    if (!await this.session.authorize() || !this.#current() || !this.#page) return;
    this.#reply(this.#page, entries);
  }
}

/** Extends the authenticated session without changing speech, model tools or printer delivery. */
export class ScreenEmojiDeviceSession extends PrinterDeviceSession {
  constructor(options) { super(options); this.screenEmoji = new ScreenEmojiChannel(this, options.screenEmojiAtlas); }
  start() { super.start(); this.screenEmoji.start(); }
  send(message) {
    const sent = super.send(message);
    if (sent && message.t === 'text') this.screenEmoji?.card(message.text);
    return sent;
  }
  handleMessage(message) { return message.t === 'emoji_get' ? this.screenEmoji.handle(message) : super.handleMessage(message); }
  close() { this.screenEmoji.close(); super.close(); }
}
