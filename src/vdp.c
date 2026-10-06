/* mdcore - Mega Drive video display processor (mode 5).
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
 *
 * Written from Sega's Genesis Software Manual and the public hardware notes
 * on VDP behaviour. One scanline is rendered at a time: both scroll planes,
 * the window and the sprites go into byte line buffers, then a priority pass
 * resolves them, including shadow/highlight. The master system mode (mode 4)
 * is not implemented.
 */
#include "vdp.h"

vdp_t vdp;
u32 vdp_fb[VDP_FB_STRIDE * VDP_FB_MAX_H];
int vdp_abgr;

static u32 colors[3][64];   /* normal, shadow, highlight */

/* Measured output levels of the three brightness ramps, 0..255. */
static const u8 level_normal[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
static const u8 level_shadow[8] = { 0, 29, 52, 70, 87, 101, 116, 130 };
static const u8 level_highlight[8] = { 130, 144, 158, 172, 187, 206, 228, 255 };

#define REG vdp.reg
#define H40 (REG[12] & 1)
#define INTERLACE2 ((REG[12] & 6) == 6)

static inline u32 vr16(u32 a)
{
    return ((u32)vdp.vram[a & 0xFFFF] << 8) | vdp.vram[(a + 1) & 0xFFFF];
}

int vdp_width(void) { return H40 ? 320 : 256; }
int vdp_height(void) { return (vdp.pal && (REG[1] & 8)) ? 240 : 224; }
int vdp_fb_height(void) { return INTERLACE2 ? vdp_height() * 2 : vdp_height(); }
int vdp_lines(void) { return vdp.pal ? 313 : 262; }

static u32 make_color(const u8 *lv, u16 c)
{
    u32 r = lv[(c >> 1) & 7], g = lv[(c >> 5) & 7], b = lv[(c >> 9) & 7];
    return vdp_abgr ? (0xFF000000u | (b << 16) | (g << 8) | r) : ((r << 16) | (g << 8) | b);
}

static void set_color(int i)
{
    u16 c = vdp.cram[i];
    colors[0][i] = make_color(level_normal, c);
    colors[1][i] = make_color(level_shadow, c);
    colors[2][i] = make_color(level_highlight, c);
}

void vdp_refresh_colors(void)
{
    int i;
    for (i = 0; i < 64; i++)
        set_color(i);
}

void vdp_reset(void)
{
    u8 pal = vdp.pal;
    memset(&vdp, 0, sizeof vdp);
    vdp.pal = pal;
    vdp_refresh_colors();
}

/* ---- memory ---- */

static u32 sat_base(void)
{
    return (u32)(REG[5] & (H40 ? 0x7E : 0x7F)) << 9;
}

static inline void vram_wb(u32 a, u8 v)
{
    u32 off;
    a &= 0xFFFF;
    vdp.vram[a] = v;
    off = a - sat_base();
    if (off < 0x400)
        vdp.sat_cache[off] = v;
}

/* ---- write FIFO ----
 * The 68000 reaches the VDP's memories only through access slots: 18 per
 * line in H40 and 16 in H32 while the display is drawing, and far more in
 * blanking. A VRAM word needs two slots (VRAM is written a byte at a time),
 * CRAM and VSRAM one. Four writes can wait in the FIFO; a fifth stalls the
 * 68000 until the oldest one has gone through. */

static int slot_active(s32 t)
{
    /* t may run past the end of the frame (or before its start) */
    int lines = vdp_lines(), line = (int)((t / 3420) % lines);
    if (line < 0)
        line += lines;
    return line < vdp_height() && (vdp.reg[1] & 0x40);
}

static s32 next_slot(s32 t)
{
    /* n slots spread evenly over the 3420 master clocks of a line */
    s32 base = (t / 3420) * 3420, pos = t - base, n, k, next;
    if (slot_active(t))
        n = H40 ? 18 : 16;
    else
        n = H40 ? 205 : 167;
    k = pos * n / 3420 + 1;
    next = (k * 3420 + n - 1) / n;
    if (next <= pos)
        next = ((k + 1) * 3420 + n - 1) / n;
    if (next >= 3420)
        return base + 3420;
    return base + next;
}

static void fifo_retire(void)
{
    int i;
    for (i = 1; i < vdp.fifo_n; i++)
        vdp.fifo_done[i - 1] = vdp.fifo_done[i];
    vdp.fifo_n--;
}

static void fifo_push(int slots)
{
    s32 now = md_now(), t;
    while (vdp.fifo_n && vdp.fifo_done[0] <= now)
        fifo_retire();
    if (vdp.fifo_n == 4) {
        md_dma_stall((u32)(vdp.fifo_done[0] - now));
        now = vdp.fifo_done[0];
        fifo_retire();
    }
    t = vdp.fifo_n ? vdp.fifo_done[vdp.fifo_n - 1] : now;
    if (t < now)
        t = now;
    while (slots--)
        t = next_slot(t);
    vdp.fifo_done[vdp.fifo_n++] = t;
}

void vdp_frame_end(s32 frame_mclk)
{
    int i;
    for (i = 0; i < vdp.fifo_n; i++)
        vdp.fifo_done[i] -= frame_mclk;
    vdp.dma_until -= frame_mclk;
    if (vdp.dma_until < -frame_mclk)
        vdp.dma_until = -frame_mclk;
}

static void store(u16 v)
{
    switch (vdp.code & 0x0F) {
    case 1:
        vram_wb(vdp.addr, (u8)(v >> 8));
        vram_wb(vdp.addr ^ 1, (u8)v);
        break;
    case 3: {
        int i = (vdp.addr >> 1) & 0x3F;
        vdp.cram[i] = v & 0x0EEE;
        set_color(i);
        break;
    }
    case 5: {
        int i = (vdp.addr >> 1) & 0x3F;
        if (i < 40)
            vdp.vsram[i] = v & 0x07FF;
        break;
    }
    }
    vdp.addr += REG[15];
}

/* Time at which n more access slots, starting after t, have gone by. */
static s32 slots_end(s32 t, u32 n)
{
    while (n--)
        t = next_slot(t);
    return t;
}

/* A 68000-to-VDP transfer takes the bus: the CPU waits until it is done. */
static void dma_slots(u32 slots)
{
    s32 now = md_now();
    md_dma_stall((u32)(slots_end(now, slots) - now));
}

static void dma_start(void)
{
    u32 len = REG[19] | ((u32)REG[20] << 8), i;
    if (!len)
        len = 0x10000;

    if (!(REG[23] & 0x80)) { /* 68000 bus to VDP */
        u32 src = ((u32)(REG[23] & 0x7F) << 17) | ((u32)REG[22] << 9) | ((u32)REG[21] << 1);
        dma_slots((vdp.code & 0x0F) == 1 ? len * 2 : len);
        for (i = 0; i < len; i++) {
            store((u16)md_dma_read16(src));
            src = (src & 0xFE0000) | ((src + 2) & 0x1FFFF);
        }
        REG[21] = (u8)(src >> 1);
        REG[22] = (u8)(src >> 9);
        REG[19] = REG[20] = 0;
    } else if (!(REG[23] & 0x40)) { /* fill: starts with the next data word */
        vdp.fill_pending = 1;
    } else { /* VRAM copy: a read and a write slot per byte, in the background */
        u32 src = REG[21] | ((u32)REG[22] << 8);
        vdp.dma_until = slots_end(md_now(), len * 2);
        for (i = 0; i < len; i++) {
            vram_wb(vdp.addr ^ 1, vdp.vram[(src ^ 1) & 0xFFFF]);
            src++;
            vdp.addr += REG[15];
        }
        REG[21] = (u8)src;
        REG[22] = (u8)(src >> 8);
        REG[19] = REG[20] = 0;
    }
}

void vdp_data_write(u16 v)
{
    vdp.pending = 0;
    fifo_push((vdp.code & 0x0F) == 1 ? 2 : 1);
    store(v);
    if (vdp.fill_pending) {
        u32 len = REG[19] | ((u32)REG[20] << 8), i;
        vdp.fill_pending = 0;
        if (!len)
            len = 0x10000;
        vdp.dma_until = slots_end(md_now(), len); /* a slot per byte, in the background */
        if ((vdp.code & 0x0F) == 1) {
            for (i = 0; i < len; i++) {
                vram_wb(vdp.addr ^ 1, (u8)(v >> 8));
                vdp.addr += REG[15];
            }
        } else {
            for (i = 0; i < len; i++)
                store(v);
        }
        REG[19] = REG[20] = 0;
    }
}

u16 vdp_data_read(void)
{
    u16 v = 0;
    vdp.pending = 0;
    switch (vdp.code & 0x0F) {
    case 0: v = (u16)vr16(vdp.addr & 0xFFFE); break;
    case 8: v = vdp.cram[(vdp.addr >> 1) & 0x3F]; break;
    case 4: v = ((vdp.addr >> 1) & 0x3F) < 40 ? vdp.vsram[(vdp.addr >> 1) & 0x3F] : 0; break;
    }
    vdp.addr += REG[15];
    return v;
}

void vdp_ctrl_write(u16 v)
{
    if (!vdp.pending) {
        if ((v & 0xC000) == 0x8000) { /* register write */
            int r = (v >> 8) & 0x1F;
            if (r < 24 && (r <= 10 || (REG[1] & 4)))
                REG[r] = (u8)v;
            vdp.code &= 0x3C; /* a register write clears CD1-0 */
            if (r == 5 || r == 12) { /* the sprite table moved: recapture it */
                u32 b = sat_base(), i;
                for (i = 0; i < 0x400; i++)
                    vdp.sat_cache[i] = vdp.vram[(b + i) & 0xFFFF];
            }
            md_irq_update();
            return;
        }
        vdp.code = (u8)((vdp.code & 0x3C) | (v >> 14));
        vdp.addr = (u16)((vdp.addr & 0xC000) | (v & 0x3FFF));
        vdp.pending = 1;
        return;
    }
    vdp.pending = 0;
    vdp.code = (u8)((vdp.code & 0x03) | ((v >> 2) & 0x3C));
    vdp.addr = (u16)((vdp.addr & 0x3FFF) | ((v & 3) << 14));
    if ((vdp.code & 0x20) && (REG[1] & 0x10)) {
        dma_start();
        vdp.code &= ~0x20;
    }
}

u16 vdp_ctrl_read(void)
{
    int pos = md_line_pos();
    s32 now = md_now();
    u16 s = 0x3400;
    while (vdp.fifo_n && vdp.fifo_done[0] <= now)
        fifo_retire();
    if (!vdp.fifo_n) s |= 0x200;
    if (vdp.fifo_n == 4) s |= 0x100;
    if (now < vdp.dma_until) s |= 0x02;
    if (vdp.vint_pending) s |= 0x80;
    if (vdp.spr_overflow) s |= 0x40;
    if (vdp.spr_collide) s |= 0x20;
    if (vdp.odd_frame) s |= 0x10;
    if (vdp.vblank || !(REG[1] & 0x40)) s |= 0x08;
    if (pos >= 2640 || pos < 80) s |= 0x04;
    if (vdp.pal) s |= 0x01;
    vdp.pending = 0;
    vdp.spr_overflow = vdp.spr_collide = 0;
    return s;
}

#ifndef HV_OFFSET_H40
#define HV_OFFSET_H40 356
#endif
#ifndef HV_OFFSET_H32
#define HV_OFFSET_H32 360
#endif

u16 vdp_hv_read(void)
{
    /* The H counter runs 0x00-0xB6 then 0xE4-0xFF in H40 (0x00-0x93 then
     * 0xE9-0xFF in H32), and V steps when H reaches 0xA5 (0x85). Its phase
     * relative to this core's line start is HV_OFFSET master clocks. */
    int span = H40 ? 211 : 171, top = H40 ? 0xB6 : 0x93, jlo = H40 ? 0xE4 : 0xE9;
    int vinc = H40 ? 0xA5 : 0x85, lines = vdp_lines();
    int hl = (md_line_pos() + (H40 ? HV_OFFSET_H40 : HV_OFFSET_H32)) * span / 3420;
    int v = vdp.line, h;
    if (hl >= vinc)
        v = (v + 1) % lines;
    if (hl >= span)
        hl -= span;
    h = hl <= top ? hl : hl - (top + 1) + jlo;
    if (vdp.pal) {
        if (REG[1] & 8) {
            if (v > 0x10A)
                v -= 49;
        } else if (v > 0x102) {
            v -= 57;
        }
    } else if (v > 0xEA) {
        v -= 6;
    }
    if (INTERLACE2)
        v = ((v << 1) & 0xFE) | ((v >> 7) & 1);
    return (u16)(((v & 0xFF) << 8) | h);
}

/* ---- rendering ---- */

/* Line buffers hold bits 0-5 colour index, bit 6 priority. They have a 16
 * pixel margin on each side so partly visible cells need no clipping. */
static u8 line_a[320 + 32], line_b[320 + 32], line_s[320 + 32];

static void draw_tile_row(u8 *dst, u32 attr, int row, int il)
{
    u32 base, px;
    u8 pal = (u8)(((attr >> 9) & 0x30) | ((attr >> 15) << 6));
    const u8 *p;
    int i;

    if (il) {
        if (attr & 0x1000)
            row ^= 15;
        base = ((attr & 0x3FF) << 6) + ((u32)row << 2);
    } else {
        if (attr & 0x1000)
            row ^= 7;
        base = ((attr & 0x7FF) << 5) + ((u32)row << 2);
    }
    p = vdp.vram + (base & 0xFFFC);
    px = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    if (attr & 0x800) {
        for (i = 0; i < 8; i++, px >>= 4)
            dst[i] = pal | (px & 15);
    } else {
        for (i = 7; i >= 0; i--, px >>= 4)
            dst[i] = pal | (px & 15);
    }
}

static void draw_plane(u8 *buf, int w, int plane, int line)
{
    static const u16 sizes[4] = { 32, 64, 32, 128 };
    int il = INTERLACE2;
    u32 nt = plane ? (u32)(REG[4] & 7) << 13 : (u32)(REG[2] & 0x38) << 10;
    int cols = sizes[REG[16] & 3], rows = sizes[(REG[16] >> 4) & 3];
    u32 hst = (u32)(REG[13] & 0x3F) << 10;
    int hl, hs, fine, c, t, ly, th = il ? 16 : 8;

    if (cols == 128)
        rows = 32;
    else if (cols == 64 && rows == 128)
        rows = 64;

    switch (REG[11] & 3) {
    case 0: hl = 0; break;
    case 1: hl = line & 7; break;
    case 2: hl = line & ~7; break;
    default: hl = line; break;
    }
    hs = (int)(vr16(hst + (u32)hl * 4 + (u32)plane * 2) & 0x3FF);
    fine = hs & 15;
    ly = il ? line * 2 + vdp.odd_frame : line;

    for (c = fine ? -1 : 0; c < w / 16; c++) {
        int x = c * 16 + fine - (fine ? 0 : 0);
        int vcol = (REG[11] & 4) ? (c < 0 ? 0 : c) : 0;
        int vs = vdp.vsram[vcol * 2 + plane] & (il ? 0x7FF : 0x3FF);
        int y = (ly + vs) & (rows * th - 1);
        int trow = y / th, prow = y & (th - 1);
        int tcol;
        if (fine)
            x = c * 16 + fine;
        tcol = ((x - hs) & (cols * 8 - 1)) >> 3;
        for (t = 0; t < 2; t++) {
            u32 attr = vr16(nt + (u32)(trow * cols + ((tcol + t) & (cols - 1))) * 2);
            draw_tile_row(buf + x + t * 8, attr, prow, il);
        }
    }
}

/* Replaces part of plane A's line with the window. */
static void draw_window(u8 *buf, int w, int line)
{
    int il = INTERLACE2;
    int wx = (REG[17] & 0x1F) * 16, wy = (REG[18] & 0x1F) * 8;
    int x0, x1, x, ly = il ? line * 2 + vdp.odd_frame : line;
    int wcols = H40 ? 64 : 32;
    u32 nt = (u32)(REG[3] & (H40 ? 0x3C : 0x3E)) << 10;
    int trow = il ? ly >> 4 : ly >> 3, prow = il ? ly & 15 : ly & 7;

    if ((REG[18] & 0x80) ? line >= wy : line < wy) {
        x0 = 0;
        x1 = w;
    } else if (REG[17] & 0x80) {
        x0 = wx;
        x1 = w;
    } else {
        x0 = 0;
        x1 = wx;
    }
    if (x1 > w)
        x1 = w;
    for (x = x0; x < x1; x += 8)
        draw_tile_row(buf + x, vr16(nt + (u32)((trow & 31) * wcols + (x >> 3)) * 2), prow, il);
}

static void draw_sprites(u8 *buf, int w, int line)
{
    int il = INTERLACE2;
    int max_spr = w == 320 ? 80 : 64, max_line = w == 320 ? 20 : 16;
    int budget = w, drawn = 0, masked = 0, seen = 0, idx = 0, n;
    int ly = il ? line * 2 + vdp.odd_frame : line, th = il ? 16 : 8;
    u32 sat = sat_base();
    int dot_overflow = 0;

    memset(buf, 0, (size_t)w);

    for (n = 0; n < max_spr; n++) {
        const u8 *c = vdp.sat_cache + idx * 8;
        int y = (((c[0] << 8) | c[1]) & (il ? 0x3FF : 0x1FF)) - (il ? 256 : 128);
        int hc = ((c[2] >> 2) & 3) + 1, vc = (c[2] & 3) + 1;
        int h = vc * th;

        if (ly >= y && ly < y + h) {
            const u8 *s = vdp.vram + ((sat + (u32)idx * 8) & 0xFFFF);
            u32 attr = ((u32)s[4] << 8) | s[5];
            int x = ((s[6] << 8) | s[7]) & 0x1FF;
            int row = ly - y, col;

            if (drawn == max_line) {
                vdp.spr_overflow = 1;
                break;
            }
            drawn++;
            if (x == 0) {
                /* a sprite at x = 0 hides the rest of the list on this line */
                if (seen || vdp.dot_overflow)
                    masked = 1;
            } else {
                seen = 1;
            }
            if (attr & 0x1000)
                row = h - 1 - row;
            for (col = 0; col < hc; col++) {
                u8 tile[8];
                int tcol = (attr & 0x800) ? hc - 1 - col : col;
                int px = x - 128 + col * 8, i;
                if (budget <= 0) {
                    dot_overflow = 1;
                    break;
                }
                budget -= 8;
                if (masked || px <= -8 || px >= w)
                    continue;
                draw_tile_row(tile, (attr & 0xE800) | (((attr & 0x7FF) + (u32)(tcol * vc + row / th)) & 0x7FF),
                              row & (th - 1), il);
                for (i = 0; i < 8; i++) {
                    int xx = px + i;
                    if (xx < 0 || xx >= w || !(tile[i] & 15))
                        continue;
                    if (buf[xx] & 15)
                        vdp.spr_collide = 1;
                    else
                        buf[xx] = tile[i];
                }
            }
            if (dot_overflow)
                break;
        }
        idx = c[3] & 0x7F;
        if (idx == 0 || idx >= max_spr)
            break;
    }
    vdp.dot_overflow = (u8)dot_overflow;
}

void vdp_render_line(int line)
{
    int w = vdp_width(), x;
    int il = INTERLACE2;
    u32 *dst = vdp_fb + (il ? line * 2 + vdp.odd_frame : line) * VDP_FB_STRIDE;
    u8 bd = REG[7] & 0x3F;
    u8 *a = line_a + 16, *b = line_b + 16, *s = line_s + 16;

    if (!(REG[1] & 0x40)) {
        u32 c = colors[0][bd];
        for (x = 0; x < w; x++)
            dst[x] = c;
        return;
    }

    draw_plane(b, w, 1, line);
    draw_plane(a, w, 0, line);
    draw_window(a, w, line);
    draw_sprites(s, w, line);

    if (!(REG[12] & 8)) {
        for (x = 0; x < w; x++) {
            u8 pa = a[x], pb = b[x], ps = s[x], pix;
            if ((ps & 0x4F) > 0x40)
                pix = ps;
            else if ((pa & 0x4F) > 0x40)
                pix = pa;
            else if ((pb & 0x4F) > 0x40)
                pix = pb;
            else if (ps & 15)
                pix = ps;
            else if (pa & 15)
                pix = pa;
            else if (pb & 15)
                pix = pb;
            else
                pix = bd;
            dst[x] = colors[0][pix & 0x3F];
        }
    } else { /* shadow / highlight */
        static const u8 ramp[3] = { 1, 0, 2 }; /* shade 0 shadow, 1 normal, 2 highlight */
        for (x = 0; x < w; x++) {
            u8 pa = a[x], pb = b[x], ps = s[x], pix;
            int hi = 1, shade = ((pa | pb) & 0x40) ? 1 : 0;
            if ((pa & 0x4F) > 0x40) {
                pix = pa;
            } else if ((pb & 0x4F) > 0x40) {
                pix = pb;
            } else {
                hi = 0;
                pix = (pa & 15) ? pa : (pb & 15) ? pb : bd;
            }
            if ((ps & 15) && ((ps & 0x40) || !hi)) {
                u8 sp = ps & 0x3F;
                if (sp == 0x3E) {
                    shade++;
                } else if (sp == 0x3F) {
                    shade = 0;
                } else {
                    pix = ps;
                    if ((ps & 0x40) || (sp & 15) == 14)
                        shade = 1;
                }
            }
            dst[x] = colors[ramp[shade]][pix & 0x3F];
        }
    }

    if (REG[0] & 0x20) { /* blank the leftmost cell */
        u32 c = colors[0][bd];
        for (x = 0; x < 8; x++)
            dst[x] = c;
    }
}
