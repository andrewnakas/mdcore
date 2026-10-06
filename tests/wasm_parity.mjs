// Runs the same scripted session through web/mdcore.wasm and prints a hash of
// the frame buffer, RAM and audio, to compare against the native build.
import { readFileSync } from 'fs';
import { createHash } from 'crypto';
const [rom, frames, script] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(readFileSync(new URL('../web/mdcore.wasm', import.meta.url)), {});
const x = instance.exports, mem = () => new Uint8Array(x.memory.buffer);
const data = readFileSync(rom);
x.md_init(0);
mem().set(data, x.md_rom_buffer());
if (!x.md_load(data.length, 0)) throw new Error('load failed');
const events = (script || '').split(',').filter(Boolean).map(s => s.split(':')).map(([f, b]) => [+f, parseInt(b, 16)]);
const h = createHash('sha1'); const t0 = performance.now();
for (let i = 0; i < +frames; i++) {
  let b = 0; for (const [f, v] of events) if (f <= i) b = v;
  x.md_set_input(0, b); x.md_run_frame();
  h.update(new Uint8Array(x.memory.buffer, x.md_audio(), x.md_audio_frames() * 4));
}
const ms = performance.now() - t0;
h.update(new Uint8Array(x.memory.buffer, x.md_framebuffer(), 320 * 224 * 4));
h.update(new Uint8Array(x.memory.buffer, x.md_ram(), 65536));
console.log(`wasm   ${h.digest('hex')}  ${(+frames / ms * 1000).toFixed(0)} fps`);
