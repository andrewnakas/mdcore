#!/usr/bin/env python3
"""Builds a ROM that plays a list of YM2612 test notes on channel 1, one per
second (key on for 40 frames, off for 20), so mdcore's FM output can be
compared against the reference test by test. Writes ymtest.gen and tests.txt."""
import os, subprocess, pathlib
HERE = pathlib.Path(__file__).resolve().parent
# The Macro Assembler AS (asl + p2bin), e.g. the Linux binaries bundled with
# sonicretro/s1disasm in build_tools/Linux-x86_64, or https://github.com/Clownacy/asl-releases
if "ASL_DIR" not in os.environ:
    raise SystemExit("set ASL_DIR to a directory containing asl and p2bin")
TOOLS = pathlib.Path(os.environ["ASL_DIR"])

BASE = dict(alg=7, fb=0, fnum=0x269, block=4,
            ops=[dict(dt=0, mul=1, tl=0x7F, ks=0, ar=31, am=0, d1r=0, d2r=0, sl=0, rr=15, ssg=0) for _ in range(4)])
BASE["ops"][3]["tl"] = 0   # operator 4 alone, full volume

def variant(name, **kw):
    import copy
    t = copy.deepcopy(BASE)
    for k, v in kw.items():
        if k.startswith("op"):           # op4_tl=..., op1_mul=...
            o, f = k[2], k[4:]
            t["ops"][int(o) - 1][f] = v
        else:
            t[k] = v
    return name, t

TESTS = [variant("sine")]
TESTS += [variant(f"tl{v}", op4_tl=v) for v in (8, 24, 48)]
TESTS += [variant(f"mul{v}", op4_mul=v) for v in (0, 2, 7, 15)]
TESTS += [variant(f"dt{v}", op4_dt=v) for v in (1, 3, 5, 7)]
TESTS += [variant(f"block{v}", block=v) for v in (1, 6)]
TESTS += [variant(f"ar{v}", op4_ar=v) for v in (10, 16, 22)]
TESTS += [variant(f"d1r{v}_sl{s}", op4_d1r=v, op4_sl=s, op4_d2r=0) for v, s in ((8, 4), (14, 8), (20, 15))]
TESTS += [variant(f"d2r{v}", op4_d1r=31, op4_sl=1, op4_d2r=v) for v in (6, 12, 18)]
TESTS += [variant(f"rr{v}", op4_rr=v) for v in (2, 5, 9)]
TESTS += [variant(f"ks{k}", op4_ks=k, op4_d1r=12, op4_sl=10, block=6) for k in (0, 3)]
# modulation: operator 1 modulating operator 4 (algorithm 0 chain via 1->2->3->4 needs all; use alg 4: 1->2 + 3->4)
for alg in range(8):
    TESTS.append(variant(f"alg{alg}", alg=alg, op1_tl=20, op2_tl=20, op3_tl=20, op4_tl=10))
for fb in (3, 5, 7):
    TESTS.append(variant(f"fb{fb}", alg=7, fb=fb, op1_tl=0, op4_tl=0x7F))
TESTS.append(variant("mod_deep", alg=4, op3_tl=0, op3_mul=1, op4_tl=0))
TESTS.append(variant("mod_mid", alg=4, op3_tl=20, op3_mul=2, op4_tl=0))
TESTS.append(variant("ssg8", op4_ssg=8, op4_d1r=20, op4_sl=15))
TESTS.append(variant("ssgA", op4_ssg=0xA, op4_d1r=20, op4_sl=15))
TESTS.append(variant("ssgD", op4_ssg=0xD, op4_d1r=20, op4_sl=15))

SLOT = [0, 8, 4, 12]   # register offset of operators 1..4

def writes(t):
    w = [(0x22, 0), (0x27, 0), (0x28, 0x00), (0xB4, 0xC0), (0xB0, (t["fb"] << 3) | t["alg"])]
    for i, o in enumerate(t["ops"]):
        s = SLOT[i]
        w += [(0x30 + s, (o["dt"] << 4) | o["mul"]), (0x40 + s, o["tl"]), (0x50 + s, (o["ks"] << 6) | o["ar"]),
              (0x60 + s, (o["am"] << 7) | o["d1r"]), (0x70 + s, o["d2r"]), (0x80 + s, (o["sl"] << 4) | o["rr"]),
              (0x90 + s, o["ssg"])]
    w += [(0xA4, (t["block"] << 3) | (t["fnum"] >> 8)), (0xA0, t["fnum"] & 0xFF)]
    return w

ASM = r"""	cpu 68000
	padding off
	org 0
	dc.l	$00FFFE00, start
	rept 22
	dc.l	trap
	endm
	dc.l	trap, trap, trap, trap, trap, trap, vint, trap
	rept 32
	dc.l	trap
	endm
	org $100
	dc.b	"SEGA MEGA DRIVE "
	org $1F0
	dc.b	"JUE             "
	org $200
trap:	rte
vint:	addq.w	#1,d6
	rte
; d0 register, d1 value
ymw:	btst	#7,(a1)
	bne.s	ymw
	move.b	d0,(a1)
	nop
	nop
	move.b	d1,1(a1)
	rts
wait:	move.w	d6,d2
.w:	cmp.w	d6,d2
	beq.s	.w
	dbf	d3,wait
	rts
start:	move	#$2700,sr
	lea	($C00004).l,a0
	move.w	#$8004,(a0)
	move.w	#$8174,(a0)
	move.w	#$100,($A11100).l
	move.w	#$100,($A11200).l
.zw:	btst	#0,($A11100).l
	bne.s	.zw
	lea	($A04000).l,a1
	moveq	#0,d6
	move	#$2000,sr
	moveq	#59,d3
	bsr.w	wait
	lea	(tests).l,a2
next:	move.b	(a2)+,d0
	beq.s	keyon
	move.b	(a2)+,d1
	bsr.w	ymw
	bra.s	next
keyon:	addq.l	#1,a2
	moveq	#$28,d0
	move.b	#$F0,d1
	bsr.w	ymw
	moveq	#39,d3
	bsr.w	wait
	moveq	#$28,d0
	moveq	#0,d1
	bsr.w	ymw
	moveq	#19,d3
	bsr.w	wait
	cmpi.b	#$FF,(a2)
	bne.s	next
.end:	bra.s	.end
tests:
"""

if __name__ == "__main__":
    out = HERE / "build"; out.mkdir(exist_ok=True)
    body = ""
    for name, t in TESTS:
        data = []
        for r, v in writes(t):
            data += [r, v]
        data += [0, 0]
        body += "\tdc.b\t" + ",".join("$%02X" % x for x in data) + "\t; " + name + "\n"
    body += "\tdc.b\t$FF\n"
    (out / "ymtest.asm").write_text(ASM + body)
    env = dict(os.environ, AS_MSGPATH=str(TOOLS))
    subprocess.run([str(TOOLS / "asl"), "-q", "-xx", "-A", "ymtest.asm"], cwd=out, check=True, env=env)
    subprocess.run([str(TOOLS / "p2bin"), "ymtest.p", "ymtest.gen"], cwd=out, check=True, env=env, stdout=subprocess.DEVNULL)
    rom = (out / "ymtest.gen").read_bytes(); rom += b"\xff" * (0x8000 - len(rom)); (out / "ymtest.gen").write_bytes(rom)
    (out / "tests.txt").write_text("\n".join(n for n, _ in TESTS) + "\n")
    print(len(TESTS), "tests ->", out / "ymtest.gen")
