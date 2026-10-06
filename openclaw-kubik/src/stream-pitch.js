import { SampleAligner } from './audio.js';

/** Incremental WSOLA followed by resampling. Keeps overlap/search/phase across network boundaries. */
export class StreamPitch {
  constructor(semitones = 0) {
    this.ratio = 2 ** (Number(semitones) / 12);
    this.active = Number.isFinite(this.ratio) && Math.abs(semitones) >= 0.05;
    this.align = new SampleAligner();
    this.n = 720; this.h = 360; this.tol = 192;
    this.x = []; this.base = 0; this.total = 0;
    this.k = 0; this.prev = 0; this.overlap = new Float64Array(this.n);
    this.y = []; this.ybase = 0; this.out = 0;
    this.win = Float64Array.from({ length: this.n }, (_, i) => 0.5 - 0.5 * Math.cos(2 * Math.PI * i / this.n));
  }
  push(chunk, final = false) {
    const pcm = this.align.push(chunk);
    if (!this.active) return pcm;
    for (let i = 0; i < pcm.length; i += 2) this.x.push(pcm.readInt16LE(i));
    this.total += pcm.length / 2;
    this.process(final);
    return this.resample(final);
  }
  sample(i) { return i < 0 || i >= this.total ? 0 : this.x[i - this.base] ?? 0; }
  position(nominal) {
    if (!this.k) return nominal;
    let best = -Infinity, selected = nominal;
    const target = this.prev + this.h;
    for (let d = -this.tol; d <= this.tol; d += 2) {
      const c = nominal + d;
      if (c < 0) continue;
      let correlation = 0;
      for (let i = 0; i < this.h; i += 2) correlation += this.sample(c + i) * this.sample(target + i);
      if (correlation > best) { best = correlation; selected = c; }
    }
    return selected;
  }
  process(final) {
    const ha = this.h / this.ratio;
    const frames = Math.max(1, Math.ceil((this.total - this.n) / ha) + 1);
    while (final ? this.k < frames : Math.round(this.k * ha) + this.tol + this.n < this.total) {
      const pos = this.position(Math.round(this.k * ha));
      for (let i = 0; i < this.n; i++) this.overlap[i] += this.sample(pos + i) * this.win[i];
      for (let i = 0; i < this.h; i++) this.y.push(this.overlap[i]);
      this.overlap.copyWithin(0, this.h); this.overlap.fill(0, this.h);
      this.prev = pos; this.k++;
      const retain = Math.max(0, Math.min(pos + this.h, Math.round(this.k * ha) - this.tol) - 2);
      const drop = Math.max(0, retain - this.base);
      this.x.splice(0, drop); this.base += drop;
    }
    if (final) for (let i = 0; i < this.n; i++) this.y.push(this.overlap[i]);
  }
  resample(final) {
    const samples = [];
    while (final ? this.out < this.total : Math.floor(this.out * this.ratio) + 1 < this.ybase + this.y.length) {
      const p = this.out * this.ratio, i = Math.floor(p) - this.ybase;
      const a = this.y[i] ?? 0, b = this.y[i + 1] ?? a;
      samples.push(Math.max(-32768, Math.min(32767, Math.round(a + (b - a) * (p % 1))))); this.out++;
    }
    const drop = Math.min(this.y.length, Math.max(0, Math.floor(this.out * this.ratio) - this.ybase));
    this.y.splice(0, drop); this.ybase += drop;
    const pcm = Buffer.alloc(samples.length * 2);
    samples.forEach((sample, i) => pcm.writeInt16LE(sample, i * 2));
    return pcm;
  }
  finish() { return this.push(Buffer.alloc(0), true); }
}
