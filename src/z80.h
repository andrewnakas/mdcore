/* mdcore - Zilog Z80 interpreter.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE. */
#ifndef MDCORE_Z80_H
#define MDCORE_Z80_H

#include "common.h"

typedef struct {
    u16 pc, sp, ix, iy;
    u16 bc, de, hl;
    u16 bc2, de2, hl2;
    u8 a, f, a2, f2;
    u8 i, r;
    u8 iff1, iff2, im;
    u8 halted;
    u8 irq;         /* level of the INT line */
    u8 ei_delay;    /* EI holds interrupts off for one more instruction */
    s32 cycles;     /* T-states executed, ever increasing within a frame */
} z80_t;

extern z80_t z80;

/* Supplied by the machine. */
u8 z80_mem_read(u16 addr);
void z80_mem_write(u16 addr, u8 val);
u8 z80_port_in(u16 port);
void z80_port_out(u16 port, u8 val);

#ifdef Z80_TRACE
extern void (*z80_trace_hook)(u16 pc);
#endif

void z80_init(void);
void z80_reset(void);
void z80_run(s32 until);    /* run until z80.cycles >= until */

#endif
