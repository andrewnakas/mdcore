# mdcore

A Sega Mega Drive / Genesis emulator core for the browser: about 77 KB of WebAssembly,
plus a small ES-module front end, under the **MIT licence**.

mdcore was written from scratch from the Motorola, Zilog and Sega manuals and public
hardware documentation. It contains no code from Genesis Plus GX, PicoDrive, BlastEm or any
other emulator. It exists because the Mega Drive cores commonly used on the web carry a
"no commercial use" clause.

- **Freestanding wasm:** no emscripten runtime, no libc, no imports. It builds with any clang
  that has the wasm32 target.
- **Your ROM stays in your browser.** The demo page reads it from a file picker and never
  sends it anywhere. Its Content-Security-Policy blocks every off-origin request.
- **Runs 25–30× faster than real time** (about 1,500 fps in Node, about 1,850 fps native, on
  Sonic 1 gameplay), so phones have headroom.

> **Status: 0.1.0, early.** Sonic the Hedgehog 1 and 2 are tested (details below). Other games
> have not been tested, and some hardware is not emulated yet (see *Known gaps*).

## Quick start

Download `mdcore-web-<version>.zip` from the [releases](../../releases), serve the folder
over HTTP, and open `index.html`:

    python3 -m http.server 8000      # then visit http://localhost:8000/

Pick your own ROM (`.md`, `.bin`, `.gen` or `.smd`). Sound starts off; turn it on with the
Sound button.

| key | button |
|---|---|
| arrow keys | D-pad |
| Z X C (or A S D) | A B C |
| Q W E | X Y Z |
| Enter | Start |
| Right Shift | Mode |

Standard gamepads are picked up automatically.

## Using it in your own page

```js
import { MDCore } from './mdcore.js';

const md = await MDCore.create(canvas, { wasmUrl: 'mdcore.wasm' });
md.load(new Uint8Array(await file.arrayBuffer()));   // e.g. a File from <input type=file>
md.start();                                           // runs at 59.92 / 49.70 fps

await md.enableAudio(0.8);        // must be called from a user gesture
md.setButtons(0, mask);           // or let the built-in keyboard/gamepad handling do it
const state = md.saveState();     // Uint8Array; md.loadState(state) to restore
const sram  = md.getSram();       // battery save, or null; md.setSram(bytes)
const ram   = md.ram();           // 64 KiB of 68000 work RAM ($FF0000-$FFFFFF), big-endian
```

Your page's CSP must allow `'wasm-unsafe-eval'` in `script-src`, or Chrome will refuse to
compile the module. `web/index.html` has a working example.

The C API is in [`src/mdcore.h`](src/mdcore.h): `md_load`, `md_run_frame`, `md_set_input`,
`md_framebuffer`, `md_audio`, `md_state_save` and so on. It does no allocation and no I/O, so
it can also be linked into a native front end. `md_run_frame` returns just before the
vertical interrupt. At that point the picture is complete, and input set before the call is
what the game's V-blank handler reads.

## Building

    ./build.sh                 # needs clang (wasm32 target) and wasm-ld (lld) on PATH
    LLVM=/path/to/llvm/bin ./build.sh

The output is `web/mdcore.wasm`. The build is deterministic. The core itself is C99 with no
dependencies, so `cc src/*.c` builds it natively as well (see `tests/headless.c`).

## What is emulated

| part | file | notes |
|---|---|---|
| 68000 | `src/m68k.c` | full instruction set, exact cycle counts, DRAM refresh |
| Z80 | `src/z80.c` | including undocumented opcodes and flags |
| VDP | `src/vdp.c` | mode 5; planes, window, line and cell scroll, sprites with per-line limits and masking, shadow/highlight, H and V interrupts, HV counter, interlace mode 2, 4-entry write FIFO with access slots, DMA (68k transfer, fill, copy) with timing and the busy flag |
| YM2612 | `src/sound.c` | 4 operators, all 8 algorithms, feedback, SSG-EG, LFO, channel 3 special mode, timers, DAC, busy flag, DAC "ladder effect" |
| SN76489 | `src/sound.c` | 3 tones and noise |
| machine | `src/md.c` | memory map, Z80 bus arbitration, banking and contention, 3- and 6-button pads, battery SRAM, SSF2 mapper, `.smd` de-interleaving, region detection, save states |

## How it was tested

**CPUs, against published test vectors.**
- 68000: 261,894 of 261,894 cases from [SingleStepTests/m68000](https://github.com/SingleStepTests/m68000)
  pass, with registers, flags, memory and cycle counts exact. The 55,606 cases that raise an
  address error are skipped, because mdcore does not model address errors.
- Z80: 1,601,037 of 1,604,000 cases from [SingleStepTests/z80](https://github.com/SingleStepTests/z80)
  pass. All 2,963 failures are undocumented flags of the repeating block-I/O instructions;
  the Mega Drive's Z80 has no I/O devices.

To run them yourself: `tests/fetch_vectors.sh && tests/run_cpu_tests.sh`.

**Whole system, against a reference emulator, as a black box.** `tests/` can build Genesis
Plus GX locally as a *test-only* oracle; it is never shipped or linked. The two emulators are
compared on:

- their outputs, frame by frame;
- timing-probe ROMs (`tests/probes/`) that measure CPU speed, VDP FIFO and DMA timing, the
  HV counter, interrupt positions, Z80 bus contention and the YM busy flag;
- a 68000 program-counter trace from the reference's debug hook.

The reference's source was not used to write mdcore. Every behaviour in mdcore was
implemented from documentation, then checked against these measurements.

Results on Sonic the Hedgehog (REV00), 22 scripted sessions of 2,600 frames each (attract
mode, plus every act, Final Zone, the special stage and the sound test, all entered through
the game's own level select):

| | frames | identical to the reference |
|---|---|---|
| picture, pixel for pixel | 57,178 | 56,776 (99.3%) |
| 68000 work RAM, byte for byte | 57,178 | 56,498 (98.8%) |

Every remaining difference is transient: mostly one-frame lags during level loading, from
small residual differences in DMA timing.

**Sound, measured numerically.**
- On Green Hill Zone's music the overall level is within 0.15 dB of the reference's
  cycle-accurate YM2612.
- The level tracks the reference with 0.988 correlation, and the mean difference is 0.54 dB.
- On 44 single-note FM tests, 33 are within ±0.3 dB.

**Native and wasm builds** produce bit-identical audio, video and RAM over a 1,500-frame
session (`tests/native_parity.c`, `tests/wasm_parity.mjs`).

**In the browser** (headless Chrome 151):
- Sonic 1 and Sonic 2 boot to their title screens and reach their first act at full speed.
- Gameplay was confirmed from the game's own RAM (level mode, not the attract demo).
- A save state round trip works.
- The page made 0 off-origin requests.

## Known gaps

- Only Sonic 1 and Sonic 2 have been tested. Expect problems in other games.
- Not emulated: address-error exceptions, EEPROM saves (some EA, Acclaim and Sega sports
  titles), the SVP chip (Virtua Racing), Sega CD and 32X, Master System mode, Game Genie and
  lock-on cartridges.
- Rendering is per line, so mid-line raster effects are not reproduced.
- YM2612 vibrato depth comes from the documented cent ranges, not the chip's shift network.
- Tested only in Chrome. Firefox, Safari, mobile browsers and real gamepads are untested.

## Layout

    src/                 the core (C99, freestanding)
    web/                 mdcore.wasm, mdcore.js, the audio worklet, and the demo page
    tools/gen_tables.py  generates src/ym2612_tables.h from closed-form maths
    tests/               CPU-vector runners, native/wasm parity, the reference-comparison
                         rig (diff_oracle.py, pc_diff.py, probes/, audio tools), browser test

Most of the comparison tools need a ROM you own, a locally built reference
(`tests/oracle/`), and The Macro Assembler AS for the probe ROMs (`ASL_DIR=...`). None of
these are included in this repository.

## Licence

MIT, see [LICENSE](LICENSE). `tests/oracle/libretro.h` is the libretro API header, under its
own MIT-style licence.
