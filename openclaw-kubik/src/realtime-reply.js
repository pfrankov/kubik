import { buildRealtimeVoiceSpeakExactMessage } from 'openclaw/plugin-sdk/realtime-voice';

/** Agent blocks can arrive during synthesis; only one provider response runs at
 * a time. Completion of the agent and completion of audio are independent. */
export class RealtimeReply {
  #items = [];
  #busy = false;
  #ended = false;
  #closed = false;
  #blocks = 0;
  constructor({ send, emotion, finish, fail, phase = () => {} }) {
    Object.assign(this, { send, emotion, finish, fail, phase });
  }
  enqueue(segments) {
    if (this.#closed) return;
    const text = segments.map(s => s.text).join(' ');
    if (!text) return;
    if (++this.#blocks > 512) throw Error('Realtime reply block limit');
    this.#items.push({ text, segments });
    this.#pump();
  }
  #pump() {
    if (this.#closed || this.#busy) return;
    const item = this.#items.shift();
    if (!item) {
      if (this.#ended) { this.close(); this.finish(); }
      return;
    }
    // Keep the first block immediate; combine plain blocks already waiting
    // behind audio to avoid a provider round trip for every short sentence.
    while (!item.segments.some(s => s.emotion) && this.#items.length) {
      const next = this.#items[0];
      if (next.segments.some(s => s.emotion) || item.text.length + next.text.length + 1 > 800) break;
      this.#items.shift(); item.text += ` ${next.text}`; item.segments.push(...next.segments);
    }
    this.#busy = true;
    try {
      for (const s of item.segments) if (s.emotion) this.emotion?.(s.emotion);
      this.phase('voice-request');
      this.send(buildRealtimeVoiceSpeakExactMessage({ text:item.text, surfaceLabel:'Kubik' }));
    } catch { this.close(); this.fail(); }
  }
  completed() {
    if (this.#closed || !this.#busy) return;
    this.#busy = false;
    // Leave the SDK's response.done handler before opening the next response.
    queueMicrotask(() => this.#pump());
  }
  end() { this.#ended = true; this.#pump(); }
  close() { this.#closed = true; this.#items.length = 0; }
}
