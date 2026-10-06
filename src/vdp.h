/* mdcore - Mega Drive video display processor (mode 5).
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE. */
#ifndef MDCORE_VDP_H
#define MDCORE_VDP_H

#include "common.h"

#define VDP_FB_STRIDE 320
#define VDP_FB_MAX_H 480

typedef struct {
    u8 vram[0x10000];
    u8 sat_cache[0x400];    /* the VDP's own copy of the sprite table's first halves */
    u16 cram[64];
    u16 vsram[40];
    u8 reg[32];
    u16 addr;
    u8 code;
    u8 pending;             /* second half of a control word expected */
    u8 fill_pending;        /* a VRAM fill waits for its data word */
    u8 vint_pending, hint_pending;
    u8 vblank;
    u8 odd_frame;
    u8 pal;                 /* 50 Hz machine */
    u8 spr_overflow, spr_collide;
    u8 dot_overflow;        /* previous line ran out of sprite pixels */
    u8 fifo_n;              /* writes queued in the 4-entry FIFO */
    s16 hint_counter;
    u16 line;
    s32 fifo_done[4];       /* master clock at which each queued write completes */
    s32 dma_until;          /* a fill or copy keeps the DMA busy flag set until then */
} vdp_t;

extern vdp_t vdp;
extern u32 vdp_fb[VDP_FB_STRIDE * VDP_FB_MAX_H];
extern int vdp_abgr;        /* 0: 0x00RRGGBB pixels, 1: 0xFFBBGGRR (canvas byte order) */

/* Supplied by the machine. */
u32 md_dma_read16(u32 addr);
void md_dma_stall(u32 mclk);    /* the 68000 loses the bus for this long */
void md_irq_update(void);
int md_line_pos(void);          /* master clocks since the start of vdp.line */
s32 md_now(void);               /* master clocks since the start of the frame */

void vdp_reset(void);
void vdp_refresh_colors(void);
void vdp_ctrl_write(u16 v);
void vdp_data_write(u16 v);
u16 vdp_ctrl_read(void);
u16 vdp_data_read(void);
u16 vdp_hv_read(void);
void vdp_render_line(int line);
void vdp_frame_end(s32 frame_mclk);
int vdp_width(void);
int vdp_height(void);       /* lines of active display: 224 or 240 */
int vdp_fb_height(void);    /* rows in the frame buffer: doubled in interlace mode 2 */
int vdp_lines(void);        /* lines per frame: 262 or 313 */

#endif
