#!/bin/sh
# Builds and runs both CPU test suites. Run tests/fetch_vectors.sh first.
set -eu
cd "$(dirname "$0")"
cc -O2 -o vectors/m68k_sst m68k_sst.c ../src/m68k.c
cc -O2 -o vectors/z80_sst z80_sst.c ../src/z80.c
vectors/m68k_sst vectors/m68000/v1/*.json.bin | tail -1
vectors/z80_sst vectors/z80.bin 0 | tail -1
