#!/usr/bin/env python3
"""Runs every probe ROM on mdcore and the reference and compares the
per-frame iteration counts the probes record in work RAM."""
import pathlib, subprocess, struct, sys, os
T = pathlib.Path(__file__).resolve().parent.parent
V = T / "vectors"; CORE = T / "oracle/gpgx/genesis_plus_gx_libretro.so"
OUT = V / "probe"; OUT.mkdir(parents=True, exist_ok=True)
empty = OUT / "none.txt"; empty.write_text("0 0\n")

def counts(cmd, prefix):
    subprocess.run(cmd + [str(empty), "", "40", str(prefix)], check=True, capture_output=True)
    ram = (pathlib.Path(str(prefix) + ".ram")).read_bytes()
    stamps.append([struct.unpack(">HH", ram[0x8000 + i * 4:0x8004 + i * 4]) for i in range(1024)])
    return [struct.unpack(">I", ram[i * 4:i * 4 + 4])[0] for i in range(10, 30)]

worst = 0
stamps = []
def first_stamp_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return f"stamp {i}: md frame {x[0]} hv {x[1]:04X} / ref frame {y[0]} hv {y[1]:04X}"
    return "all stamps equal"
for rom in sorted((T / "probes/build").glob("*.gen")):
    if len(sys.argv) > 1 and rom.stem not in sys.argv[1:]:
        continue
    m = counts([str(V / "trace"), str(rom), "41"], OUT / (rom.stem + ".md"))
    g = counts([str(V / "oracle"), str(CORE), str(rom), "41"], OUT / (rom.stem + ".gx"))
    am, ag = sum(m) / len(m), sum(g) / len(g)
    d = (am - ag) / ag * 100 if ag else float("inf")
    worst = max(worst, abs(d))
    sd = first_stamp_diff(stamps[-2], stamps[-1]); stamps.clear()
    print(f"{rom.stem:15s} mdcore {am:9.1f}/frame  ref {ag:9.1f}/frame  diff {d:+6.2f}%   md {m[:3]} ref {g[:3]}  {sd}")
print(f"worst {worst:.2f}%")
