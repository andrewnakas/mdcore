#!/usr/bin/env python3
"""Writes input scripts that use Sonic 1's own level select (title screen:
Up, Down, Left, Right, then hold A and press Start) to reach every act, then
run right and jump. Nothing is poked into memory: these are button presses."""
import pathlib
OUT = pathlib.Path(__file__).resolve().parent / "scripts"
U, D, L, R, B, C, A, S = 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80
ENTRIES = ["ghz1", "ghz2", "ghz3", "mz1", "mz2", "mz3", "syz1", "syz2", "syz3", "lz1", "lz2", "lz3",
           "slz1", "slz2", "slz3", "sbz1", "sbz2", "sbz3", "fz", "ss", "sound"]

def script(index, play_frames=1500):
    ev = [(0, 0)]
    t = 600
    for b in (U, D, L, R):                      # the level select code
        ev += [(t, b), (t + 4, 0)]; t += 10
    ev += [(t, A), (t + 10, A | S), (t + 14, 0)]; t += 120   # hold A, press Start
    for _ in range(index):                      # move down the menu
        ev += [(t, D), (t + 4, 0)]; t += 12
    ev += [(t + 10, S), (t + 14, 0)]; t += 200    # choose
    # play: hold right, tap C every 40 frames
    for k in range(play_frames // 40):
        ev += [(t, R), (t + 30, R | C), (t + 36, R)]; t += 40
    return ev, t

if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    for i, name in enumerate(ENTRIES):
        ev, end = script(i)
        (OUT / f"ls_{name}.txt").write_text(f"# level select -> {name}; run to frame {end}\n" +
                                            "".join(f"{f} {m:x}\n" for f, m in ev))
    print("wrote", len(ENTRIES), "scripts")
