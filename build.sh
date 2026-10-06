#!/bin/sh
# Builds web/mdcore.wasm with a stock clang + wasm-ld. No emscripten runtime,
# no libc: the module is freestanding and exports the md_* API directly.
set -eu
cd "$(dirname "$0")"
# Any clang with the wasm32 target and wasm-ld (lld) on PATH will do.
# Set LLVM=/path/to/llvm/bin to use a specific toolchain, e.g. emsdk's upstream/bin.
if [ -n "${LLVM:-}" ]; then CC="$LLVM/clang"; PATH="$LLVM:$PATH"; export PATH; fi
CC="${CC:-clang}"
SRC="src/md.c src/m68k.c src/z80.c src/vdp.c src/sound.c src/libc.c"
EXPORTS="md_init md_rom_buffer md_load md_reset md_run_frame md_set_input md_set_six_button
 md_framebuffer md_width md_height md_is_pal md_fps_x1000 md_audio md_audio_frames
 md_audio_rate_x1000 md_ram md_sram md_sram_size md_sram_dirty md_state_size
 md_state_save md_state_load md_state_buffer md_set_audio_filter"
EXP=""
for e in $EXPORTS; do EXP="$EXP -Wl,--export=$e"; done
mkdir -p web
"$CC" --target=wasm32-unknown-unknown -O3 -flto -nostdlib -ffreestanding \
  -fno-builtin-memcpy -fno-builtin-memset -Wall -Wextra \
  -Wl,--no-entry -Wl,--lto-O3 -Wl,--initial-memory=16777216 -Wl,--max-memory=16777216 \
  -Wl,-z,stack-size=262144 -Wl,--strip-all $EXP \
  -o web/mdcore.wasm $SRC
ls -l web/mdcore.wasm
if command -v sha1sum >/dev/null; then sha1sum web/mdcore.wasm; fi
