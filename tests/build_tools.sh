#!/bin/sh
# Rebuilds the native test drivers against the current core.
set -e
cd "$(dirname "$0")"
mkdir -p vectors
SRC="../src/md.c ../src/m68k.c ../src/z80.c ../src/vdp.c ../src/sound.c"
cc -O2 -Wall -o vectors/trace trace.c $SRC
cc -O2 -Wall -DM68K_TRACE -DZ80_TRACE -o vectors/trace_pc trace.c $SRC
cc -O2 -Wall -o vectors/oracle oracle/oracle.c -ldl
