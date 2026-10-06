#!/usr/bin/env python3
"""Runs a script through mdcore (trace) and the reference (oracle), reports the
first frame where video or work RAM diverges, and dumps both sides of it.

    diff_oracle.py ROM SCRIPT FRAMES [--out DIR] [--ignore-ram ADDR-ADDR,...]
"""
import argparse, os, subprocess, sys, pathlib

HERE = pathlib.Path(__file__).resolve().parent
BIN = pathlib.Path(os.environ.get("MDTEST_BIN", HERE / "vectors"))
CORE = HERE / "oracle" / "gpgx" / "genesis_plus_gx_libretro.so"


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.split("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom"); ap.add_argument("script"); ap.add_argument("frames", type=int)
    ap.add_argument("--out", default=str(BIN / "diff"))
    ap.add_argument("--audio", action="store_true", help="also write audio for audio_compare.py")
    ap.add_argument("--frame", type=int, help="inspect this frame instead of the first divergence")
    a = ap.parse_args()
    out = pathlib.Path(a.out); out.mkdir(parents=True, exist_ok=True)
    tag = pathlib.Path(a.script).stem
    am = str(out / f"{tag}.md.raw") if a.audio else ""
    ag = str(out / f"{tag}.gx.raw") if a.audio else ""
    md = run([str(BIN / "trace"), a.rom, str(a.frames), a.script, am])
    gx = run([str(BIN / "oracle"), str(CORE), a.rom, str(a.frames), a.script, ag])
    fv = fr = None; nv = nr = 0
    for lm, lg in zip(md, gx):
        if not lm or not lg:
            continue
        m, g = lm.split(), lg.split()
        f = int(m[0])
        if f == 0:
            continue  # the reference reports its boot placeholder for frame 0
        if m[1:3] != g[1:3]:
            nv += 1; fv = f if fv is None else fv
        if m[3] != g[3]:
            nr += 1; fr = f if fr is None else fr
    print(f"{tag}: {a.frames} frames, video differs on {nv}, RAM differs on {nr}")
    first = min(x for x in (fv, fr, None) if x is not None) if (fv or fr) else None
    if a.frame is not None:
        first = a.frame
    if first is None:
        print("IDENTICAL"); return 0
    print(f"first divergence: frame {first} (video {fv}, ram {fr})")
    run([str(BIN / "trace"), a.rom, str(first + 1), a.script, "", str(first), str(out / f"{tag}.md")])
    run([str(BIN / "oracle"), str(CORE), a.rom, str(first + 1), a.script, "", str(first), str(out / f"{tag}.gx")])
    rm = (out / f"{tag}.md.ram").read_bytes(); rg = (out / f"{tag}.gx.ram").read_bytes()
    diffs = [i for i in range(0x10000) if rm[i] != rg[i]]
    print(f"RAM bytes differing: {len(diffs)}")
    for i in diffs[:24]:
        print(f"  $FF{i:04X}: mdcore {rm[i]:02X}  ref {rg[i]:02X}")
    pm = (out / f"{tag}.md.ppm").read_bytes(); pg = (out / f"{tag}.gx.ppm").read_bytes()
    hm, hg = pm.split(b"\n", 1), pg.split(b"\n", 1)
    if hm[0] != hg[0]:
        print(f"frame size differs: {hm[0]} vs {hg[0]}")
    else:
        w = int(hm[0].split()[1])
        bad = [i // 3 for i in range(0, len(hm[1]), 3) if hm[1][i:i + 3] != hg[1][i:i + 3]]
        if bad:
            ys = sorted({p // w for p in bad})
            print(f"pixels differing: {len(bad)}, rows {ys[0]}..{ys[-1]} ({len(ys)} rows), first at x={bad[0] % w} y={bad[0] // w}")
    print(f"dumps: {out}/{tag}.md.* and {tag}.gx.*")
    return 1


if __name__ == "__main__":
    sys.exit(main())
