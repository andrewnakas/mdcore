/* mdcore - YM2612 FM synthesiser and SN76489 PSG.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
 *
 * The FM part follows the published descriptions of the OPN2: a 20-bit phase
 * counter per operator, a quarter-wave log-sine ROM feeding an exponent ROM,
 * and a 10-bit envelope generator clocked every third sample. Detune values
 * come from the Yamaha application manual. Vibrato depth is computed from the
 * documented cent ranges rather than from the chip's shift network, and the
 * DAC non-linearity ("ladder effect") is not modelled.
 */
#include "sound.h"
#include "ym2612_tables.h"

ym2612_t ym;
psg_t psg;
u32 sound_mask;
int sound_lowpass;

enum { EG_ATT, EG_DEC, EG_SUS, EG_REL };

static s32 mix_dc[2], mix_lp[2];

static const u8 dt_tab[4][32] = {
    { 0 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8 },
    { 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16 },
    { 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22 },
};

static const u8 slot_map[4] = { 0, 2, 1, 3 };           /* register slot -> operator */
static const u8 ch3_map[4] = { 1, 2, 0, 0 };            /* operator -> channel 3 frequency slot */
static const u8 lfo_period[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };
static const u8 am_shift[4] = { 8, 3, 1, 0 };
/* peak vibrato, 2^(cents/1200) - 1 in 0.16 fixed point, for 0 3.4 6.7 10 14 20 40 80 cents */
static const u16 pm_depth[8] = { 0, 129, 254, 380, 532, 761, 1532, 3099 };
static const u8 pm_wave[8] = { 0, 50, 98, 142, 181, 213, 237, 251 };
static const u16 psg_vol[16] = { 4000, 3178, 2524, 2004, 1592, 1264, 1004, 798, 634, 504, 400, 318, 252, 200, 160, 0 };

/* ---- YM2612 ---- */

static u8 keycode(u32 fnum, u32 block)
{
    u32 f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
    return (u8)((block << 2) | (f11 << 1) | ((f11 & (f10 | f9 | f8)) | ((f11 ^ 1) & f10 & f9 & f8)));
}

static void op_freq(int c, int i, u32 *fnum, u32 *block)
{
    if (c == 2 && ym.ch3_mode && i < 3) {
        *fnum = ym.fnum3[ch3_map[i]];
        *block = ym.block3[ch3_map[i]];
    } else {
        *fnum = ym.ch[c].fnum;
        *block = ym.ch[c].block;
    }
}

/* f12 is the F-number doubled, so that vibrato can add half steps. */
static u32 phase_inc(const ym_op_t *op, u32 f12, u32 block)
{
    u32 b = (f12 << block) >> 2;
    u32 dt = dt_tab[op->dt & 3][op->kc];
    b = (op->dt & 4) ? b - dt : b + dt;
    b &= 0x1FFFF;
    b = op->mul ? b * op->mul : b >> 1;
    return b & 0xFFFFF;
}

static void update_freq(int c)
{
    int i;
    for (i = 0; i < 4; i++) {
        ym_op_t *op = &ym.ch[c].op[i];
        u32 fnum, block;
        op_freq(c, i, &fnum, &block);
        op->kc = keycode(fnum, block);
        op->inc = phase_inc(op, fnum << 1, block);
    }
}

static u32 pm_inc(int c, int i)
{
    ym_op_t *op = &ym.ch[c].op[i];
    u32 fnum, block, step = ym.lfo_cnt >> 2, q = step & 7, delta;
    op_freq(c, i, &fnum, &block);
    if (step & 8)
        q ^= 7;
    delta = ((fnum << 1) * pm_depth[ym.ch[c].fms] * pm_wave[q]) >> 24;
    return phase_inc(op, (step & 16) ? (fnum << 1) - delta : (fnum << 1) + delta, block);
}

static u32 eg_rate(const ym_op_t *op, u32 r)
{
    u32 rate;
    if (r == 0)
        return 0;
    rate = 2 * r + (op->kc >> (3 - op->rs));
    return rate > 63 ? 63 : rate;
}

static u32 eg_increment(u32 rate, u32 i)
{
    static const u8 lo[4][8] = {
        { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    };
    static const u8 hi[4][8] = {
        { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 1, 0, 0, 0, 1 }, { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 },
    };
    if (rate < 2)
        return 0;
    if (rate < 6)
        return lo[0][i];
    if (rate < 8)
        return lo[2][i];
    if (rate < 48)
        return lo[rate & 3][i];
    if (rate >= 60)
        return 8;
    return (1u << ((rate - 48) >> 2)) << hi[rate & 3][i];
}

static void key_set(ym_op_t *op, int on)
{
    if (on) {
        if (!op->keyon) {
            op->keyon = 1;
            op->phase = 0;
            op->ssg_inv = 0;
            if (eg_rate(op, op->ar) >= 62) {
                op->att = 0;
                op->state = op->sl ? EG_DEC : EG_SUS;
            } else {
                op->state = EG_ATT;
            }
        }
    } else if (op->keyon) {
        op->keyon = 0;
        if ((op->ssg & 8) && op->state != EG_REL && (op->ssg_inv ^ ((op->ssg >> 2) & 1)))
            op->att = (0x200 - op->att) & 0x3FF;
        op->state = EG_REL;
    }
}

/* SSG-EG: once the envelope has fallen past the half-way point it repeats,
 * holds or flips, depending on the mode bits. */
static void ssg_update(ym_op_t *op)
{
    if (op->att < 0x200)
        return;
    if ((op->ssg & 2) && (!(op->ssg & 1) || !op->ssg_inv))
        op->ssg_inv ^= 1;
    if (!(op->ssg & 2) && !(op->ssg & 1))
        op->phase = 0;
    if (op->state != EG_ATT) {
        if (op->state != EG_REL && !(op->ssg & 1)) {
            if (eg_rate(op, op->ar) >= 62) {
                op->att = 0;
                op->state = op->sl ? EG_DEC : EG_SUS;
            } else {
                op->state = EG_ATT;
            }
        } else if (op->state == EG_REL || !(op->ssg_inv ^ ((op->ssg >> 2) & 1))) {
            op->att = 0x3FF;
        }
    }
}

static void eg_step(ym_op_t *op)
{
    u32 r, rate, shift, inc, sl;
    s32 a = op->att;

    switch (op->state) {
    case EG_ATT: r = op->ar; break;
    case EG_DEC: r = op->d1r; break;
    case EG_SUS: r = op->d2r; break;
    default: r = op->rr * 2u + 1; break;
    }
    rate = eg_rate(op, r);
    shift = rate >= 48 ? 0 : 11 - (rate >> 2);
    if (ym.eg_cnt & ((1u << shift) - 1))
        return;
    inc = eg_increment(rate, (ym.eg_cnt >> shift) & 7);

    if (op->state == EG_ATT) {
        if (rate >= 62)
            a = 0;
        else
            a += (~a * (s32)inc) >> 4;
        if (a <= 0) {
            a = 0;
            op->state = op->sl ? EG_DEC : EG_SUS;
        }
    } else {
        if (op->ssg & 8) {
            if (a < 0x200)
                a += (s32)inc * 4;
        } else {
            a += (s32)inc;
        }
        if (a > 0x3FF)
            a = 0x3FF;
        if (op->state == EG_DEC) {
            sl = op->sl == 15 ? 0x3E0 : (u32)op->sl << 5;
            if ((u32)a >= sl)
                op->state = EG_SUS;
        }
    }
    op->att = (u16)a;
}

/* One operator: 10-bit phase and 10-bit attenuation in, signed 14 bits out. */
static inline s32 op_calc(u32 phase, u32 env)
{
    u32 idx = phase & 0xFF, a;
    s32 v;
    if (phase & 0x100)
        idx ^= 0xFF;
    a = ym_logsin[idx] + (env << 2);
    if (a >= 0x1A00)
        return 0;
    v = (s32)(((ym_exp[(a & 0xFF) ^ 0xFF] | 0x400u) << 2) >> (a >> 8));
    return (phase & 0x200) ? -v : v;
}

static s32 chan_calc(int c, int eg_tick, u32 am)
{
    ym_ch_t *ch = &ym.ch[c];
    u32 env[4], ph[4];
    s32 o1, o2, o3, o4, old2, out, m;
    int i, pm = ym.lfo_en && ch->fms;

    for (i = 0; i < 4; i++) {
        ym_op_t *op = &ch->op[i];
        u32 a;
        if (op->ssg & 8)
            ssg_update(op);
        if (eg_tick)
            eg_step(op);
        a = op->att;
        if ((op->ssg & 8) && op->state != EG_REL && (op->ssg_inv ^ ((op->ssg >> 2) & 1)))
            a = (0x200 - a) & 0x3FF;
        a += (u32)op->tl << 3;
        if (op->am)
            a += am >> am_shift[ch->ams];
        env[i] = a > 0x3FF ? 0x3FF : a;
        ph[i] = op->phase >> 10;
        op->phase = (op->phase + (pm ? pm_inc(c, i) : op->inc)) & 0xFFFFF;
    }

    m = ch->fb ? (ch->fb1 + ch->fb2) >> (10 - ch->fb) : 0;
    o1 = op_calc(ph[0] + (u32)m, env[0]);
    ch->fb2 = ch->fb1;
    ch->fb1 = (s16)o1;
    old2 = ch->op[1].out;

    /* The chip evaluates operators in the order 1, 3, 2, 4 and some results
     * reach the next operator a sample late: operator 3 always hears the
     * previous output of operator 2; in algorithms 0 and 3 operator 2 hears
     * the previous output of operator 1, and in algorithm 3 operator 4 takes
     * the previous outputs of 2 and 3. Measured with tests/probes/ym against
     * a cycle-level reference. fb2 is operator 1's previous output. */
    switch (ch->alg) {
    case 0:
        o3 = op_calc(ph[2] + (u32)(old2 >> 1), env[2]);
        o2 = op_calc(ph[1] + (u32)(ch->fb2 >> 1), env[1]);
        o4 = op_calc(ph[3] + (u32)(o3 >> 1), env[3]);
        out = o4;
        break;
    case 1:
        o3 = op_calc(ph[2] + (u32)((o1 + old2) >> 1), env[2]);
        o2 = op_calc(ph[1], env[1]);
        o4 = op_calc(ph[3] + (u32)(o3 >> 1), env[3]);
        out = o4;
        break;
    case 2:
        o3 = op_calc(ph[2] + (u32)(old2 >> 1), env[2]);
        o2 = op_calc(ph[1], env[1]);
        o4 = op_calc(ph[3] + (u32)((o1 + o3) >> 1), env[3]);
        out = o4;
        break;
    case 3:
        o4 = op_calc(ph[3] + (u32)((old2 + ch->op[2].out) >> 1), env[3]);
        o3 = op_calc(ph[2], env[2]);
        o2 = op_calc(ph[1] + (u32)(ch->fb2 >> 1), env[1]);
        out = o4;
        break;
    case 4:
        o3 = op_calc(ph[2], env[2]);
        o2 = op_calc(ph[1] + (u32)(o1 >> 1), env[1]);
        o4 = op_calc(ph[3] + (u32)(o3 >> 1), env[3]);
        out = o2 + o4;
        break;
    case 5:
        o3 = op_calc(ph[2] + (u32)(o1 >> 1), env[2]);
        o2 = op_calc(ph[1] + (u32)(o1 >> 1), env[1]);
        o4 = op_calc(ph[3] + (u32)(o1 >> 1), env[3]);
        out = o2 + o3 + o4;
        break;
    case 6:
        o3 = op_calc(ph[2], env[2]);
        o2 = op_calc(ph[1] + (u32)(o1 >> 1), env[1]);
        o4 = op_calc(ph[3], env[3]);
        out = o2 + o3 + o4;
        break;
    default:
        o3 = op_calc(ph[2], env[2]);
        o2 = op_calc(ph[1], env[1]);
        o4 = op_calc(ph[3], env[3]);
        out = o1 + o2 + o3 + o4;
        break;
    }
    ch->op[1].out = (s16)o2;
    ch->op[2].out = (s16)o3;

    if (out > 8191)
        out = 8191;
    else if (out < -8192)
        out = -8192;
    return out >> 5; /* the DAC keeps nine bits */
}

/* The YM2612's 9-bit DAC has a step between -1 and 0 (the "ladder
 * effect"): every output is pushed away from zero by a fixed amount, which
 * is what makes quiet notes louder and adds a little crossover distortion. */
#ifndef FM_GAIN
#define FM_GAIN 34
#endif
#ifndef LADDER
#define LADDER 12
#endif
static inline s32 dac_out(s32 v)
{
    return v * 4 + (v >= 0 ? LADDER : -LADDER);
}

void ym_reset(void)
{
    int c, i;
    memset(&ym, 0, sizeof ym);
    for (c = 0; c < 6; c++) {
        ym.ch[c].left = ym.ch[c].right = 1;
        for (i = 0; i < 4; i++) {
            ym.ch[c].op[i].att = 0x3FF;
            ym.ch[c].op[i].state = EG_REL;
        }
        update_freq(c);
    }
    ym.eg_cnt = 1;
}

void ym_write(int port, u8 v)
{
    u8 a;
    int c, i;

    if (!(port & 1)) {
        ym.addr = v;
        ym.part = (port >> 1) & 1;
        return;
    }
    a = ym.addr;

    if (a < 0x30) {
        if (ym.part)
            return;
        switch (a) {
        case 0x22:
            ym.lfo_en = (v >> 3) & 1;
            ym.lfo_freq = v & 7;
            if (!ym.lfo_en) {
                ym.lfo_cnt = 0;
                ym.lfo_div = 0;
            }
            break;
        case 0x24:
            ym.timer_a = (u16)((ym.timer_a & 3) | (v << 2));
            break;
        case 0x25:
            ym.timer_a = (u16)((ym.timer_a & 0x3FC) | (v & 3));
            break;
        case 0x26:
            ym.timer_b = v;
            break;
        case 0x27:
            if ((v & 1) && !(ym.timer_ctrl & 1))
                ym.timer_a_cnt = ym.timer_a;
            if ((v & 2) && !(ym.timer_ctrl & 2)) {
                ym.timer_b_cnt = ym.timer_b;
                ym.timer_b_div = 0;
            }
            if (v & 0x10)
                ym.status &= ~1;
            if (v & 0x20)
                ym.status &= ~2;
            ym.timer_ctrl = v;
            ym.ch3_mode = v >> 6;
            update_freq(2);
            break;
        case 0x28:
            c = v & 3;
            if (c == 3)
                break;
            if (v & 4)
                c += 3;
            for (i = 0; i < 4; i++)
                key_set(&ym.ch[c].op[i], v & (0x10 << i));
            break;
        case 0x2A:
            ym.dac = v;
            break;
        case 0x2B:
            ym.dac_en = v >> 7;
            break;
        }
        return;
    }

    c = a & 3;
    if (c == 3)
        return;
    if (a < 0xA0) {
        ym_op_t *op = &ym.ch[c + (ym.part ? 3 : 0)].op[slot_map[(a >> 2) & 3]];
        switch (a & 0xF0) {
        case 0x30: op->dt = (v >> 4) & 7; op->mul = v & 15; break;
        case 0x40: op->tl = v & 0x7F; break;
        case 0x50: op->rs = v >> 6; op->ar = v & 31; break;
        case 0x60: op->am = v >> 7; op->d1r = v & 31; break;
        case 0x70: op->d2r = v & 31; break;
        case 0x80: op->sl = v >> 4; op->rr = v & 15; break;
        case 0x90: op->ssg = v & 15; break;
        }
        update_freq(c + (ym.part ? 3 : 0));
        return;
    }
    switch (a & 0xFC) {
    case 0xA0:
        c += ym.part ? 3 : 0;
        ym.ch[c].fnum = (u16)(((ym.ch[c].fnum_latch & 7) << 8) | v);
        ym.ch[c].block = ym.ch[c].fnum_latch >> 3;
        update_freq(c);
        break;
    case 0xA4:
        ym.ch[c + (ym.part ? 3 : 0)].fnum_latch = v & 0x3F;
        break;
    case 0xA8:
        if (!ym.part) {
            ym.fnum3[c] = (u16)(((ym.latch3[c] & 7) << 8) | v);
            ym.block3[c] = ym.latch3[c] >> 3;
            update_freq(2);
        }
        break;
    case 0xAC:
        if (!ym.part)
            ym.latch3[c] = v & 0x3F;
        break;
    case 0xB0:
        c += ym.part ? 3 : 0;
        ym.ch[c].alg = v & 7;
        ym.ch[c].fb = (v >> 3) & 7;
        break;
    case 0xB4:
        c += ym.part ? 3 : 0;
        ym.ch[c].left = v >> 7;
        ym.ch[c].right = (v >> 6) & 1;
        ym.ch[c].ams = (v >> 4) & 3;
        ym.ch[c].fms = v & 7;
        break;
    }
}

u8 ym_read(void)
{
    return ym.status;
}

/* ---- SN76489 ---- */

void psg_write(u8 v)
{
    int ch, vol;
    if (v & 0x80) {
        psg.latch = (v >> 4) & 7;
        ch = psg.latch >> 1;
        vol = psg.latch & 1;
        if (vol) {
            psg.vol[ch] = v & 15;
        } else if (ch < 3) {
            psg.period[ch] = (u16)((psg.period[ch] & 0x3F0) | (v & 15));
        } else {
            psg.noise_ctrl = v & 7;
            psg.lfsr = 0x8000;
        }
    } else {
        ch = psg.latch >> 1;
        vol = psg.latch & 1;
        if (vol) {
            psg.vol[ch] = v & 15;
        } else if (ch < 3) {
            psg.period[ch] = (u16)((psg.period[ch] & 0x00F) | ((v & 0x3F) << 4));
        } else {
            psg.noise_ctrl = v & 7;
            psg.lfsr = 0x8000;
        }
    }
}

static u32 psg_tick(void)
{
    u32 level = 0, np;
    int ch;
    for (ch = 0; ch < 3; ch++) {
        if (psg.period[ch] <= 1) {
            psg.out[ch] = 1; /* too fast to hear: a steady level, used for sample playback */
        } else if (psg.counter[ch] <= 1) {
            psg.counter[ch] = psg.period[ch];
            psg.out[ch] ^= 1;
        } else {
            psg.counter[ch]--;
        }
        if (psg.out[ch] && !(sound_mask & (0x40u << ch)))
            level += psg_vol[psg.vol[ch]];
    }
    if (psg.counter[3] <= 1) {
        np = psg.noise_ctrl & 3;
        psg.counter[3] = np == 3 ? psg.period[2] : (u16)(0x10 << np);
        if (psg.counter[3] == 0)
            psg.counter[3] = 1;
        psg.out[3] ^= 1;
        if (psg.out[3]) {
            u32 fb = (psg.noise_ctrl & 4) ? ((psg.lfsr ^ (psg.lfsr >> 3)) & 1) : (psg.lfsr & 1);
            psg.lfsr = (u16)((psg.lfsr >> 1) | (fb << 15));
        }
    } else {
        psg.counter[3]--;
    }
    if ((psg.lfsr & 1) && !(sound_mask & 0x200))
        level += psg_vol[psg.vol[3]];
    return level;
}

/* ---- mixer ---- */

void sound_reset(void)
{
    int i;
    ym_reset();
    memset(&psg, 0, sizeof psg);
    for (i = 0; i < 4; i++)
        psg.vol[i] = 15;
    psg.lfsr = 0x8000;
    mix_dc[0] = mix_dc[1] = 0;
    mix_lp[0] = mix_lp[1] = 0;
}

void sound_render(s16 *out, int count)
{
    while (count-- > 0) {
        s32 l = 0, r = 0, p, v;
        u32 am = 0, n, i;
        int c, eg_tick = 0;

        if (ym.lfo_en) {
            if (++ym.lfo_div >= lfo_period[ym.lfo_freq]) {
                ym.lfo_div = 0;
                ym.lfo_cnt = (ym.lfo_cnt + 1) & 0x7F;
            }
            am = ((ym.lfo_cnt & 0x40) ? (ym.lfo_cnt & 0x3Fu) : (~ym.lfo_cnt & 0x3Fu)) << 1;
        }
        if (++ym.eg_div >= 3) {
            ym.eg_div = 0;
            if (++ym.eg_cnt >= 4096)
                ym.eg_cnt = 1;
            eg_tick = 1;
        }
        if (ym.timer_ctrl & 1) {
            if (++ym.timer_a_cnt >= 1024) {
                ym.timer_a_cnt = ym.timer_a;
                if (ym.timer_ctrl & 4)
                    ym.status |= 1;
            }
        }
        if (ym.timer_ctrl & 2) {
            if (++ym.timer_b_div >= 16) {
                ym.timer_b_div = 0;
                if (++ym.timer_b_cnt >= 256) {
                    ym.timer_b_cnt = ym.timer_b;
                    if (ym.timer_ctrl & 8)
                        ym.status |= 2;
                }
            }
        }

        for (c = 0; c < 6; c++) {
            v = chan_calc(c, eg_tick, am);
            if (c == 5 && ym.dac_en)
                v = ((s32)ym.dac - 0x80) * 2;
            if (sound_mask & (1u << c))
                continue;
            v = dac_out(v);
            if (ym.ch[c].left)
                l += v;
            if (ym.ch[c].right)
                r += v;
        }

        /* the PSG runs at 1/16 of the Z80 clock: 4.2 steps per output sample */
        psg.frac += 275251;
        n = psg.frac >> 16;
        psg.frac &= 0xFFFF;
        p = 0;
        for (i = 0; i < n; i++)
            p += (s32)psg_tick();
        p /= (s32)n;

        l = l * FM_GAIN / 4 + p;
        r = r * FM_GAIN / 4 + p;

        /* remove DC, then a gentle one-pole low-pass like the console's output stage */
        mix_dc[0] += l - (mix_dc[0] >> 10);
        mix_dc[1] += r - (mix_dc[1] >> 10);
        l -= mix_dc[0] >> 10;
        r -= mix_dc[1] >> 10;
        if (sound_lowpass) {
            mix_lp[0] += ((l << 8) - mix_lp[0]) * 3 / 8;
            mix_lp[1] += ((r << 8) - mix_lp[1]) * 3 / 8;
            l = mix_lp[0] >> 8;
            r = mix_lp[1] >> 8;
        }

        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        *out++ = (s16)l;
        *out++ = (s16)r;
    }
}
