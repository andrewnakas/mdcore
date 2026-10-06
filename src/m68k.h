/* mdcore - Motorola 68000 interpreter.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE. */
#ifndef MDCORE_M68K_H
#define MDCORE_M68K_H

#include "common.h"

typedef struct {
    u32 d[8];
    u32 a[8];       /* a[7] is the active stack pointer */
    u32 sp_other;   /* the inactive one: USP while supervisor, SSP while user */
    u32 pc;
    u32 ir_pc;      /* address of the instruction being executed */
    u8 x, n, z, v, c;
    u8 s, t, imask;
    u8 irq;         /* interrupt level currently asserted, 0 = none */
    u8 stopped;
    u8 tas_writes;  /* 0 on a Mega Drive: the TAS write cycle never reaches the bus */
    u8 refresh;     /* 1 on a Mega Drive: DRAM refresh steals bus time */
    s32 cycles;     /* CPU clocks executed, ever increasing within a frame */
    s32 refresh_next;
} m68k_t;

extern m68k_t m68k;

/* 64 KiB page tables for direct access. Bytes are stored big-endian. A NULL
 * entry routes the access to the handlers below. */
extern u8 *m68k_rmap[256];
extern u8 *m68k_wmap[256];

/* Supplied by the machine. */
u32 m68k_bus_read8(u32 addr);
u32 m68k_bus_read16(u32 addr);
void m68k_bus_write8(u32 addr, u32 val);
void m68k_bus_write16(u32 addr, u32 val);
void m68k_irq_ack(int level);

#ifdef M68K_TRACE
extern void (*m68k_trace_hook)(u32 pc);
#endif

void m68k_reset(void);
void m68k_run(s32 until);   /* run until m68k.cycles >= until */
u32 m68k_get_sr(void);
void m68k_set_sr(u32 sr);

#endif
