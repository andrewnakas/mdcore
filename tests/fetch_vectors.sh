#!/bin/sh
# Fetches the CPU test vectors used by tests/m68k_sst.c and tests/z80_sst.c.
# They are third-party test data (SingleStepTests, MIT) and are not vendored.
set -eu
cd "$(dirname "$0")"
mkdir -p vectors && cd vectors
[ -d m68000 ] || git clone -q --depth 1 https://github.com/SingleStepTests/m68000.git
if [ ! -f z80.bin ]; then
  [ -d z80 ] || git clone -q --depth 1 https://github.com/SingleStepTests/z80.git
  python3 ../z80_sst_pack.py z80/v1 z80.bin && rm -rf z80   # 1.6 GB of JSON -> 214 MB
fi
