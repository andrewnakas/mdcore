// mdcore - browser front end for mdcore.wasm.
// Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
//
//   const md = await MDCore.create(canvas, { wasmUrl: 'mdcore.wasm' });
//   md.load(new Uint8Array(await file.arrayBuffer()));
//   md.start();
//
// Audio stays off until enableAudio() is called from a user gesture.

export const Buttons = {
  UP: 0x001, DOWN: 0x002, LEFT: 0x004, RIGHT: 0x008, B: 0x010, C: 0x020,
  A: 0x040, START: 0x080, Z: 0x100, Y: 0x200, X: 0x400, MODE: 0x800,
};

const DEFAULT_KEYS = {
  ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT',
  KeyA: 'A', KeyS: 'B', KeyD: 'C', KeyZ: 'A', KeyX: 'B', KeyC: 'C',
  KeyQ: 'X', KeyW: 'Y', KeyE: 'Z', Enter: 'START', ShiftRight: 'MODE',
};

// Standard gamepad mapping: face buttons to A/B/C, shoulders to X/Y/Z.
const PAD_MAP = [
  [0, 'A'], [1, 'B'], [2, 'C'], [3, 'Y'], [4, 'X'], [5, 'Z'], [8, 'MODE'], [9, 'START'],
  [12, 'UP'], [13, 'DOWN'], [14, 'LEFT'], [15, 'RIGHT'],
];

export class MDCore {
  static async create(canvas, opts = {}) {
    const url = opts.wasmUrl || new URL('mdcore.wasm', import.meta.url);
    const res = await fetch(url);
    const { instance } = await WebAssembly.instantiate(await res.arrayBuffer(), {});
    return new MDCore(canvas, instance.exports, opts);
  }

  constructor(canvas, exports, opts) {
    this.x = exports;
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d', { alpha: false });
    this.keys = { ...DEFAULT_KEYS, ...(opts.keys || {}) };
    this.held = [0, 0];
    this.touch = 0;
    this.running = false;
    this.loaded = false;
    this.frames = 0;
    this.audio = null;
    this.onSramChange = opts.onSramChange || null;
    this.x.md_init(1); // ABGR: the bytes land in canvas RGBA order
    this._onKey = (e) => this._key(e);
    window.addEventListener('keydown', this._onKey);
    window.addEventListener('keyup', this._onKey);
  }

  get memory() { return new Uint8Array(this.x.memory.buffer); }

  load(rom, region = 0) {
    if (rom.length > 0x800000) throw new Error('ROM is larger than 8 MiB');
    this.memory.set(rom, this.x.md_rom_buffer());
    if (!this.x.md_load(rom.length, region)) throw new Error('not a Mega Drive ROM');
    this.loaded = true;
    this.frames = 0;
    this._resize();
  }

  reset() { this.x.md_reset(); }

  // Work RAM (64 KiB, big-endian, as the 68000 sees $FF0000-$FFFFFF).
  ram() { return new Uint8Array(this.x.memory.buffer, this.x.md_ram(), 0x10000); }

  sramSize() { return this.x.md_sram_size(); }
  getSram() {
    const n = this.x.md_sram_size();
    return n ? new Uint8Array(this.x.memory.buffer, this.x.md_sram(), n).slice() : null;
  }
  setSram(data) {
    const n = Math.min(this.x.md_sram_size(), data.length);
    if (n) this.memory.set(data.subarray(0, n), this.x.md_sram());
  }

  saveState() {
    const buf = this.x.md_state_buffer();
    const n = this.x.md_state_save(buf);
    return new Uint8Array(this.x.memory.buffer, buf, n).slice();
  }
  loadState(data) {
    if (data.length !== this.x.md_state_size()) return false;
    this.memory.set(data, this.x.md_state_buffer());
    return !!this.x.md_state_load(this.x.md_state_buffer(), data.length);
  }

  setButtons(port, mask) { this.held[port] = mask; }
  setTouchButtons(mask) { this.touch = mask; }

  _key(e) {
    const name = this.keys[e.code];
    if (!name || e.target instanceof HTMLInputElement) return;
    const bit = Buttons[name];
    if (e.type === 'keydown') this.held[0] |= bit; else this.held[0] &= ~bit;
    e.preventDefault();
  }

  _pads() {
    const out = [0, 0];
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    let port = 0;
    for (const p of pads) {
      if (!p || port > 1) continue;
      let m = 0;
      for (const [i, n] of PAD_MAP) if (p.buttons[i] && p.buttons[i].pressed) m |= Buttons[n];
      const [ax, ay] = [p.axes[0] || 0, p.axes[1] || 0];
      if (ax < -0.5) m |= Buttons.LEFT;
      if (ax > 0.5) m |= Buttons.RIGHT;
      if (ay < -0.5) m |= Buttons.UP;
      if (ay > 0.5) m |= Buttons.DOWN;
      out[port++] = m;
    }
    return out;
  }

  step() {
    const pads = this._pads();
    this.x.md_set_input(0, this.held[0] | this.touch | pads[0]);
    this.x.md_set_input(1, this.held[1] | pads[1]);
    this.x.md_run_frame();
    this.frames++;
    if (this.audio) this._pushAudio();
    if (this.onSramChange && this.x.md_sram_dirty()) this.onSramChange(this.getSram());
  }

  draw() {
    const w = this.x.md_width(), h = this.x.md_height();
    if (this.canvas.width !== w || this.canvas.height !== h) this._resize();
    const src = new Uint8ClampedArray(this.x.memory.buffer, this.x.md_framebuffer(), 320 * h * 4);
    if (!this._img || this._img.height !== h) this._img = new ImageData(320, h);
    this._img.data.set(src);
    this.ctx.putImageData(this._img, 0, 0, 0, 0, w, h);
  }

  _resize() {
    this.canvas.width = this.x.md_width();
    this.canvas.height = this.x.md_height();
  }

  start() {
    if (this.running || !this.loaded) return;
    this.running = true;
    const period = 1e6 / this.x.md_fps_x1000();
    let last = performance.now(), acc = 0;
    const tick = (now) => {
      if (!this.running) return;
      acc += Math.min(now - last, 100);
      last = now;
      let n = 0;
      while (acc >= period && n < 4) { this.step(); acc -= period; n++; }
      if (n) this.draw();
      this._raf = requestAnimationFrame(tick);
    };
    this._raf = requestAnimationFrame(tick);
  }

  stop() {
    this.running = false;
    cancelAnimationFrame(this._raf);
  }

  // Must be called from a user gesture. Resamples 53.3 kHz to the device rate.
  async enableAudio(volume = 1) {
    if (this.audio) { this.audio.gain.gain.value = volume; return; }
    const ac = new AudioContext({ latencyHint: 'interactive' });
    await ac.audioWorklet.addModule(new URL('audio-worklet.js', import.meta.url));
    const node = new AudioWorkletNode(ac, 'mdcore-audio', { outputChannelCount: [2] });
    const gain = ac.createGain();
    gain.gain.value = volume;
    node.connect(gain).connect(ac.destination);
    this.audio = { ac, node, gain, pos: 0, last: [0, 0] };
  }

  setVolume(v) { if (this.audio) this.audio.gain.gain.value = v; }

  async disableAudio() {
    if (!this.audio) return;
    const a = this.audio;
    this.audio = null;
    await a.ac.close();
  }

  _pushAudio() {
    const a = this.audio, n = this.x.md_audio_frames();
    const src = new Int16Array(this.x.memory.buffer, this.x.md_audio(), n * 2);
    const ratio = this.x.md_audio_rate_x1000() / 1000 / a.ac.sampleRate;
    // Position p is in input samples; -1 is the last sample of the previous frame.
    const outN = n ? Math.max(0, Math.ceil((n - 1 - a.pos) / ratio)) : 0;
    const l = new Float32Array(outN), r = new Float32Array(outN);
    let p = a.pos;
    for (let i = 0; i < outN; i++, p += ratio) {
      const k = Math.floor(p), f = p - k;
      const l0 = k >= 0 ? src[2 * k] : a.last[0], r0 = k >= 0 ? src[2 * k + 1] : a.last[1];
      l[i] = (l0 + (src[2 * k + 2] - l0) * f) / 32768;
      r[i] = (r0 + (src[2 * k + 3] - r0) * f) / 32768;
    }
    a.pos = p - n;
    if (n) a.last = [src[2 * n - 2], src[2 * n - 1]];
    a.node.port.postMessage({ l, r }, [l.buffer, r.buffer]);
  }

  destroy() {
    this.stop();
    this.disableAudio();
    window.removeEventListener('keydown', this._onKey);
    window.removeEventListener('keyup', this._onKey);
  }
}
