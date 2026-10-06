#!/usr/bin/env python3
"""Builds tiny timing-probe ROMs. Each one runs a loop body forever, counting
iterations in D7; the vertical interrupt stores the count for that frame in
work RAM at $FF0000 + 4*frame (64 frames) and clears it. Running a probe on
two emulators and comparing the counts measures their timing as a black box.
Assembled with AS (asl/p2bin) from the s1disasm build tools."""
import os, subprocess, sys, pathlib
HERE = pathlib.Path(__file__).resolve().parent
# The Macro Assembler AS (asl + p2bin), e.g. the Linux binaries bundled with
# sonicretro/s1disasm in build_tools/Linux-x86_64, or https://github.com/Clownacy/asl-releases
if "ASL_DIR" not in os.environ:
    raise SystemExit("set ASL_DIR to a directory containing asl and p2bin")
TOOLS = pathlib.Path(os.environ["ASL_DIR"])

PROBES = {
    "empty":     ("", ""),
    "nops":      ("", "\tnop\n" * 8),
    "ramread":   ("\tlea\t($FF8000).l,a1\n", "\tmove.w\t(a1),d0\n" * 4),
    "romread":   ("\tlea\t($000200).l,a1\n", "\tmove.w\t(a1),d0\n" * 4),
    "ramexec":   ("COPY", "\tnop\n" * 8),
    "vdpwrite":  ("\tlea\t($C00000).l,a6\n\tmove.l\t#$40000000,4(a6)\n", "\tmove.w\td0,(a6)\n" * 4),
    "vdpwrite_blank": ("\tlea\t($C00000).l,a6\n\tmove.w\t#$8134,4(a6)\n\tmove.l\t#$40000000,4(a6)\n", "\tmove.w\td0,(a6)\n" * 4),
    "z80busreq": ("\tmove.w\t#$100,($A11100).l\n", "\tmove.w\t(a1),d0\n" * 4),
    "dbra":      ("", "\tmoveq\t#15,d1\n.l:\tdbf\td1,.l\n"),
    "mulu":      ("", "\tmulu.w\td7,d0\n"),
    # 2048 back-to-back HV counter reads at power-on, one every 84 master clocks
    "hvscan":    ("\tlea\t($C00008).l,a1\n\tlea\t($FFA000).l,a3\n\tmove.w\t#255,d2\n.s:\n" + "\tmove.w\t(a1),(a3)+\n" * 8 + "\tdbf\td2,.s\n", ""),
    "hvscan_h32": ("\tmove.w\t#$8C00,(a0)\n\tlea\t($C00008).l,a1\n\tlea\t($FFA000).l,a3\n\tmove.w\t#255,d2\n.s:\n" + "\tmove.w\t(a1),(a3)+\n" * 8 + "\tdbf\td2,.s\n", ""),
    # HV counter at entry to the vertical and horizontal interrupt handlers
    "irq_nops":  ("\tmove.w\t#$8A10,(a0)\n\tmove.w\t#$8014,(a0)\n", "\tnop\n"),
    "irq_h32":   ("\tmove.w\t#$8C00,(a0)\n\tmove.w\t#$8A10,(a0)\n\tmove.w\t#$8014,(a0)\n", "\tnop\n"),
    "irq_mulu":  ("\tmove.w\t#$8A10,(a0)\n\tmove.w\t#$8014,(a0)\n", "\tmulu.w\td0,d1\n\tdivu.w\t#3,d1\n"),
    # Sonic 1's checksum loop, over this probe's own 32 KiB
    "checksum":  ("", "\tlea\t($200).l,a0\n\tmove.l\t#$7FFE,d0\n.l:\tadd.w\t(a0)+,d1\n\tcmp.l\ta0,d0\n\tbhs.s\t.l\n"),
    # a 64 KiB fill with the display off, as at boot
    "fill64k":   ("\tlea\t($C00004).l,a0\n\tmove.w\t#$8134,(a0)\n\tmove.w\t#$8F01,(a0)\n",
                  "\tmove.l\t#$93FF94FF,(a0)\n\tmove.w\t#$9780,(a0)\n\tmove.l\t#$40000080,(a0)\n\tmove.w\t#0,-4(a0)\n.w:\tbtst\t#1,1(a0)\n\tbne.s\t.w\n"),
    # each iteration starts a 4 KiB DMA and spins on the busy flag
    "dmafill":   ("\tlea\t($C00004).l,a0\n\tmove.w\t#$8F01,(a0)\n",
                  "\tmove.l\t#$93FF940F,(a0)\n\tmove.w\t#$9780,(a0)\n\tmove.l\t#$40000080,(a0)\n\tmove.w\t#0,-4(a0)\n.w:\tbtst\t#1,1(a0)\n\tbne.s\t.w\n"),
    "dmafill_blank": ("\tlea\t($C00004).l,a0\n\tmove.w\t#$8134,(a0)\n\tmove.w\t#$8F01,(a0)\n",
                  "\tmove.l\t#$93FF940F,(a0)\n\tmove.w\t#$9780,(a0)\n\tmove.l\t#$40000080,(a0)\n\tmove.w\t#0,-4(a0)\n.w:\tbtst\t#1,1(a0)\n\tbne.s\t.w\n"),
    "dmacopy":   ("\tlea\t($C00004).l,a0\n\tmove.w\t#$8F01,(a0)\n",
                  "\tmove.l\t#$93FF9407,(a0)\n\tmove.l\t#$95009680,(a0)\n\tmove.w\t#$97C0,(a0)\n\tmove.l\t#$400000C0,(a0)\n.w:\tbtst\t#1,1(a0)\n\tbne.s\t.w\n"),
    "dma68k_vram": ("\tlea\t($C00004).l,a0\n\tmove.w\t#$8F02,(a0)\n",
                  "\tmove.l\t#$93009402,(a0)\n\tmove.l\t#$95009600,(a0)\n\tmove.w\t#$977F,(a0)\n\tmove.l\t#$40000080,(a0)\n"),
    "dma68k_cram": ("\tlea\t($C00004).l,a0\n\tmove.w\t#$8F02,(a0)\n",
                  "\tmove.l\t#$93409400,(a0)\n\tmove.l\t#$95009600,(a0)\n\tmove.w\t#$977F,(a0)\n\tmove.l\t#$C0000080,(a0)\n"),
}

Z80_START = """	move.w	#$100,($A11100).l
	move.w	#$100,($A11200).l
.zw:	btst	#0,($A11100).l
	bne.s	.zw
	lea	(zprog).l,a1
	lea	($A00000).l,a2
	moveq	#zprog_end-zprog-1,d0
.zc:	move.b	(a1)+,(a2)+
	dbf	d0,.zc
	move.w	#0,($A11200).l
	moveq	#40,d0
.zd:	dbf	d0,.zd
	move.w	#$100,($A11200).l
	move.w	#0,($A11100).l
"""
# Z80 programs: ld hl,nnnn / loop: ld a,(hl) / jr loop
PROBES["z80bankread"] = ("ZBANK" + Z80_START, "\tnop\n")   # reads cartridge ROM via the bank window
LATE = "\tmove.w\t#$8154,(a0)\n\tmove.l\t#100000,d0\n.dl:\tsubq.l\t#1,d0\n\tbne.s\t.dl\n"
PROBES["irq_late_enable"] = (LATE + "\tmove.w\t#$8174,(a0)\n", "\tnop\n")
PROBES["irq_late_enable_read"] = (LATE + "\tmove.w\t(a0),d1\n\tmove.w\t#$8174,(a0)\n", "\tnop\n")
ZGRAB = "\tmove.w\t#$100,($A11100).l\n\tmove.w\t#$100,($A11200).l\n.zg:\tbtst\t#0,($A11100).l\n\tbne.s\t.zg\n\tlea\t($A04000).l,a1\n"
PROBES["ym_busy"] = (ZGRAB, "\tmove.b\t#$30,(a1)\n\tmove.b\t#0,1(a1)\n.yb:\tbtst\t#7,(a1)\n\tbne.s\t.yb\n")
PROBES["ym_nobusy"] = (ZGRAB, "\tmove.b\t#$30,(a1)\n\tmove.b\t#0,1(a1)\n\tbtst\t#7,(a1)\n")
PROBES["ym_addr_busy"] = (ZGRAB, "\tmove.b\t#$30,(a1)\n.yb:\tbtst\t#7,(a1)\n\tbne.s\t.yb\n")
PROBES["zs_speed"] = ("ZSPEED" + Z80_START, "\tnop\n")
PROBES["zs_ym"] = ("ZYM" + Z80_START, "\tnop\n")
PROBES["zs_int"] = ("ZINT" + Z80_START, "\tnop\n")
PROBES["z80count"] = ("ZCOUNT" + Z80_START, "\tnop\n")
PROBES["z80countown"] = ("ZCOUNTOWN" + Z80_START, "\tnop\n")
PROBES["z80ownram"] = ("ZOWN" + Z80_START, "\tnop\n")      # control: reads its own RAM

TEMPLATE = r"""	cpu 68000
	padding off
	org 0
	dc.l	$00FFFE00, start
	rept 22
	dc.l	trap
	endm
	dc.l	trap, trap, trap, trap, hint, trap, vint, trap
	rept 32
	dc.l	trap
	endm
	org $100
	dc.b	"SEGA MEGA DRIVE "
	dc.b	"(C)PROBE 2026.OCT"
	org $150
	dc.b	"TIMING PROBE"
	org $180
	dc.b	"GM 00000000-00"
	org $1A0
	dc.l	0, $7FFF, $FF0000, $FFFFFF
	org $1F0
	dc.b	"JUE             "
	org $200
trap:	rte
hint:	rte
vint:	addq.w	#1,d6
	move.l	d7,(a5)+
	cmpa.l	#$FF0100,a5
	bcs.s	.ok
	lea	($FF0000).l,a5
.ok:	moveq	#0,d7
	rte
start:	move	#$2700,sr
	lea	($C00004).l,a0
	move.w	#$8004,(a0)
	move.w	#$8174,(a0)
	move.w	#$8C81,(a0)
	move.w	#$8F02,(a0)
	lea	($FF0000).l,a5
	lea	($FF8000).l,a4
	moveq	#0,d7
	moveq	#0,d6
SETUP
	move	#$2000,sr
	jmp	(LOOPADDR).l
loop:
BODY
	addq.l	#1,d7
	cmpa.l	#$FF9000,a4
	bcc.s	.nostamp
	move.w	d6,(a4)+
	move.w	($C00008).l,(a4)+
.nostamp:
	bra.w	loop
loopend:
"""

COPY = """	lea	(loop).l,a2
	lea	($FFC000).l,a3
	move.w	#(loopend-loop)/2-1,d0
.c:	move.w	(a2)+,(a3)+
	dbf	d0,.c
	move.w	#$6000,-4(a3)
	move.w	#$FFC000-($FFC000+loopend-loop-4)-2,-2(a3)
"""

IRQ_STAMP = """vint:	move.w	($C00008).l,-(sp)
	move.w	(sp)+,(a4)+
	addq.w	#1,d6
	cmpa.l	#$FF9000,a4
	bcs.s	.k
	lea	($FF8000).l,a4
.k:	rte
hint:	move.w	($C00008).l,-(sp)
	move.w	(sp)+,(a2)+
	cmpa.l	#$FFA000,a2
	bcs.s	.k2
	lea	($FF9000).l,a2
.k2:	rte
"""

ZPROGS = {
    # ld hl,8000h / loop: ld a,(hl) / jr loop
    "ZBANK": "zprog:\tdc.b\t$21,$00,$80,$7E,$18,$FD\nzprog_end:\n\talign 2\n",
    # ld hl,1000h / loop: ld a,(hl) / jr loop
    "ZOWN":  "zprog:\tdc.b\t$21,$00,$10,$7E,$18,$FD\nzprog_end:\n\talign 2\n",
    # ld hl,8000h / ld bc,20000 / loop: ld a,(hl) / dec bc / ld a,b / or c / jr nz,loop / halt
    "ZCOUNT": "zprog:\tdc.b\t$21,$00,$80,$01,$20,$4E,$7E,$0B,$78,$B1,$20,$FA,$76\nzprog_end:\n\talign 2\n",
    # same loop without touching the bank: ld a,(hl) reads Z80 RAM
    "ZCOUNTOWN": "zprog:\tdc.b\t$21,$00,$10,$01,$20,$4E,$7E,$0B,$78,$B1,$20,$FA,$76\nzprog_end:\n\talign 2\n",
}

ZS_VINT = """vint:	move.w	#$100,($A11100).l
.zv:	btst	#0,($A11100).l
	bne.s	.zv
	move.b	($A01001).l,(a4)+
	move.b	($A01000).l,(a4)+
	move.b	($A01003).l,(a4)+
	move.b	($A01002).l,(a4)+
	move.w	#0,($A11100).l
	cmpa.l	#$FF9000,a4
	bcs.s	.k
	lea	($FF8000).l,a4
.k:	rte
"""

# ld hl,0 / loop: inc hl / ld (1000h),hl / jr loop
ZPROGS["ZSPEED"] = "zprog:\tdc.b\t$21,$00,$00,$23,$22,$00,$10,$18,$FA\nzprog_end:\n\talign 2\n"
# the same with a YM2612 write: ld (4000h),a
ZPROGS["ZYM"] = "zprog:\tdc.b\t$21,$00,$00,$23,$22,$00,$10,$32,$00,$40,$18,$F7\nzprog_end:\n\talign 2\n"
# di / im 1 / ld sp,1F00h / ld hl,0 / ei / loop: inc hl / ld (1000h),hl / jr loop
# 38h: push af / ld a,(1002h) / inc a / ld (1002h),a / pop af / ei / reti
ZPROGS["ZINT"] = ("zprog:\tdc.b\t$F3,$ED,$56,$31,$00,$1F,$21,$00,$00,$FB,$23,$22,$00,$10,$18,$FA\n\tdc.b\t"
                  + ",".join(["$00"] * (0x38 - 16)) + "\n\tdc.b\t$F5,$3A,$02,$10,$3C,$32,$02,$10,$F1,$FB,$ED,$4D\nzprog_end:\n\talign 2\n")

def build(name, setup, body):
    asm = TEMPLATE
    if name.startswith("zs_"):
        a = asm.index("vint:"); b = asm.index("start:")
        asm = asm[:a] + ZS_VINT + asm[b:]
        asm = asm.replace("BODY\n\taddq.l\t#1,d7\n\tcmpa.l\t#$FF9000,a4\n\tbcc.s\t.nostamp\n\tmove.w\td6,(a4)+\n\tmove.w\t($C00008).l,(a4)+\n.nostamp:", "BODY")
    for k, v in sorted(ZPROGS.items(), key=lambda kv: -len(kv[0])):
        if setup.startswith(k):
            setup = setup[len(k):]
            asm = asm.replace("loopend:\n", "loopend:\n" + v)
    if name.startswith("irq"):
        a = asm.index("hint:\trte"); b = asm.index("start:")
        asm = asm[:a] + IRQ_STAMP + asm[b:]
        asm = asm.replace("BODY\n\taddq.l\t#1,d7\n\tcmpa.l\t#$FF9000,a4\n\tbcc.s\t.nostamp\n\tmove.w\td6,(a4)+\n\tmove.w\t($C00008).l,(a4)+\n.nostamp:", "BODY")
        setup = "\tlea\t($FF9000).l,a2\n" + setup
    asm = asm.replace("BODY", body.rstrip("\n") or "\tnop")
    if setup == "COPY":
        asm = asm.replace("SETUP", COPY).replace("LOOPADDR", "$FFC000")
    else:
        asm = asm.replace("SETUP", setup.rstrip("\n") or "\tnop").replace("LOOPADDR", "loop")
    out = HERE / "build"; out.mkdir(exist_ok=True)
    src = out / f"{name}.asm"; src.write_text(asm)
    env = dict(os.environ, AS_MSGPATH=str(TOOLS))
    subprocess.run([str(TOOLS / "asl"), "-q", "-xx", "-A", "-L", src.name], cwd=out, check=True, env=env)
    subprocess.run([str(TOOLS / "p2bin"), f"{name}.p", f"{name}.gen"], cwd=out, check=True, env=env,
                   stdout=subprocess.DEVNULL)
    rom = (out / f"{name}.gen").read_bytes()
    rom += b"\xff" * (0x8000 - len(rom))
    (out / f"{name}.gen").write_bytes(rom)
    return out / f"{name}.gen"

if __name__ == "__main__":
    for n, (s, b) in PROBES.items():
        if len(sys.argv) > 1 and n not in sys.argv[1:]:
            continue
        print(build(n, s, b))
