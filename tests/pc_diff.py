#!/usr/bin/env python3
"""Logs every executed 68000 PC on mdcore and on the hook-enabled reference
build, then walks both logs and reports where control flow first differs.
A lone repeated PC on the reference side is its hook re-reporting an
instruction after a time-slice break, and is skipped.

    pc_diff.py ROM SCRIPT FRAMES"""
import array, os, subprocess, sys, pathlib
T = pathlib.Path(__file__).resolve().parent
V = T / "vectors"
rom, script, frames = sys.argv[1], sys.argv[2], sys.argv[3]
env = dict(os.environ, TRACE_PCLOG=str(V / "pc_md.bin"))
subprocess.run([str(V / "trace_pc"), rom, frames, script], env=env, check=True, stdout=subprocess.DEVNULL)
env["TRACE_PCLOG"] = str(V / "pc_gx.bin")
subprocess.run([str(V / "oracle"), str(T / "oracle/gpgx-hook/genesis_plus_gx_libretro.so"), rom, frames, script],
               env=env, check=True, stdout=subprocess.DEVNULL)

CPU = os.environ.get("CPU", "68k")   # 68k or z80

def load(p):
    a = array.array("I"); a.frombytes(p.read_bytes())
    pcs, fr, f, i = [], [], -1, 0
    want_z80 = CPU == "z80"
    while i < len(a):
        if a[i] == 0xFFFFFFFF:
            f = a[i + 1]; i += 2; continue
        if bool(a[i] & 0x40000000) == want_z80:
            pcs.append(a[i] & 0xFFFFFF); fr.append(f)
        i += 1
    return pcs, fr

m, mf = load(V / "pc_md.bin"); g, gf = load(V / "pc_gx.bin")
i = j = 0; events = 0; polls = 0
limit = int(os.environ.get("EVENTS", "8"))
while i < len(m) and j < len(g) and events < limit:
    if m[i] == g[j]:
        i += 1; j += 1; continue
    best = None
    for tot in range(1, 1600):
        for di in range(0, tot + 1):
            dj = tot - di
            if m[i + di:i + di + 64] == g[j + dj:j + dj + 64] and len(m[i + di:i + di + 64]) == 64:
                best = (di, dj); break
        if best: break
    ctx = set(m[max(0, i - 12):i]) | (set(m[i + best[0]:i + best[0] + 12]) if best else set())
    extra = m[i:i + best[0]] + g[j:j + best[1]] if best else None
    if best and set(extra) <= ctx:
        polls += 1                      # only extra or missing iterations of a polling loop
        i += best[0]; j += best[1]
        continue
    events += 1
    print(f"event {events}: md insn {i} frame {mf[i]} / ref insn {j} frame {gf[j]}"
          + (f": md +{best[0]} {[hex(x) for x in m[i:i+best[0]]][:12]}  ref +{best[1]} {[hex(x) for x in g[j:j+best[1]]][:12]}" if best else ": NO RESYNC"))
    if not best:
        print("  md ", " ".join("%X" % x for x in m[max(0, i - 8):i + 24]))
        print("  ref", " ".join("%X" % x for x in g[max(0, j - 8):j + 24]))
        sys.exit(1)
    i += best[0]; j += best[1]
print(f"compared {i} / {j} instructions over {frames} frames: {events} control-flow event(s), {polls} polling-count difference(s)")
