/* mdcore - YM2612 FM synthesiser and SN76489 PSG.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE. */
#ifndef MDCORE_SOUND_H
#define MDCORE_SOUND_H

#include "common.h"

typedef struct {
    /* registers */
    u8 mul, dt, tl, rs, ar, am, d1r, d2r, sl, rr, ssg;
    /* envelope generator */
    u8 state, keyon, ssg_inv;
    u16 att;        /* 10-bit attenuation, 0 = loudest */
    /* phase generator */
    u32 phase;      /* 20 bits */
    u32 inc;        /* phase step without vibrato */
    u8 kc;          /* key code of the frequency this operator uses */
    u8 pad[3];
    s16 out;        /* latest 14-bit output */
    s16 pad2;
} ym_op_t;

typedef struct {
    ym_op_t op[4];      /* logical operators 1..4 */
    u16 fnum;
    u8 block, fnum_latch;
    u8 alg, fb, ams, fms;
    u8 left, right;
    s16 fb1, fb2;       /* operator 1 history for feedback */
} ym_ch_t;

typedef struct {
    ym_ch_t ch[6];
    u16 fnum3[3];       /* channel 3 per-operator frequencies */
    u8 block3[3], latch3[3];
    u8 addr, part;
    u8 ch3_mode;
    u8 lfo_en, lfo_freq, lfo_cnt;
    u8 dac_en, dac;
    u8 status;
    u8 timer_ctrl;
    u8 eg_div;
    u8 timer_b_div;
    u16 lfo_div;
    u16 eg_cnt;
    u16 timer_a, timer_a_cnt;
    u16 timer_b, timer_b_cnt;
} ym2612_t;

typedef struct {
    u16 period[4];
    u16 counter[4];
    u8 vol[4];
    u8 out[4];
    u8 latch;
    u8 noise_ctrl;
    u16 lfsr;
    u32 frac;       /* PSG clock position within the output sample, 16.16 */
} psg_t;

extern ym2612_t ym;
extern psg_t psg;
extern int sound_lowpass;  /* model 1 style output filter, off by default */
extern u32 sound_mask;   /* tooling: bit n mutes FM channel n (0-5), bit 6+n PSG channel n */

void sound_reset(void);
void ym_reset(void);
void ym_write(int port, u8 val);    /* port 0..3 */
u8 ym_read(void);
void psg_write(u8 val);

/* Renders count stereo frames (interleaved signed 16-bit) at the native rate,
 * one frame per 144 YM2612 clocks. */
void sound_render(s16 *out, int count);

#endif
