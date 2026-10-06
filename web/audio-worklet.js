// mdcore - audio output ring buffer.
// Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
class MDCoreAudio extends AudioWorkletProcessor {
  constructor() {
    super();
    this.size = 16384;
    this.l = new Float32Array(this.size);
    this.r = new Float32Array(this.size);
    this.rd = 0;
    this.wr = 0;
    this.port.onmessage = (e) => {
      const { l, r } = e.data;
      // keep latency bounded: drop the backlog if the emulator ran ahead
      if (this.fill() > this.size / 2) this.rd = this.wr;
      for (let i = 0; i < l.length; i++) {
        this.l[this.wr] = l[i];
        this.r[this.wr] = r[i];
        this.wr = (this.wr + 1) % this.size;
      }
    };
  }

  fill() { return (this.wr - this.rd + this.size) % this.size; }

  process(_inputs, outputs) {
    const [ol, or] = outputs[0];
    for (let i = 0; i < ol.length; i++) {
      if (this.rd !== this.wr) {
        ol[i] = this.l[this.rd];
        or[i] = this.r[this.rd];
        this.rd = (this.rd + 1) % this.size;
      } else {
        ol[i] = or[i] = 0;
      }
    }
    return true;
  }
}
registerProcessor('mdcore-audio', MDCoreAudio);
