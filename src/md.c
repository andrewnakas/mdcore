/* mdcore - the Mega Drive itself: buses, timing, controllers, cartridge.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
 *
 * Everything is timed in master clock units (53.69 MHz NTSC). A scanline is
 * 3420 of them; the 68000 divides the clock by 7, the Z80 by 15 and the
 * YM2612 produces one sample every 1008. The two CPUs are run in short
 * alternating slices and resynchronised whenever one touches the other.
 */
#include "mdcore.h"
#include "m68k.h"
#include "z80.h"
#include "vdp.h"
#include "sound.h"

#define LINE_MCLK 3420
/* Each line is drawn in one go at this point of it, so mid-frame palette
 * and scroll changes land on the same line as on hardware. */
#ifndef RENDER_MCLK
#define RENDER_MCLK (-500)
#endif
#ifndef HINT_MCLK
#define HINT_MCLK 2300
#endif
/* how long the Z80 sees its INT line asserted */
#ifndef Z80_INT_MCLK
#define Z80_INT_MCLK (171 * 15)
#endif
#ifndef VINT_MCLK
#define VINT_MCLK 3040
#endif
#define SLICE_MCLK 488
#define SAMPLE_MCLK 1008

/* Each Z80 access to the 68000 bus makes the Z80 wait about 3.4 of its
 * cycles and holds the 68000 off the bus for about 74 master clocks. */
/* After a data write the YM2612 is busy for 32 of its internal cycles. */
#define YM_BUSY_MCLK (32 * 6 * 7)

#ifndef ZBANK_68K_MCLK
#define ZBANK_68K_MCLK 72
#endif

/* Where in the frame the 68000 leaves reset. On hardware this depends on
 * when power came up; a fixed value makes runs reproducible, and this one is
 * the phase of the reference emulator used by tests/diff_oracle.py. */
#ifndef POWER_ON_MCLK
#define POWER_ON_MCLK 653300
#endif

typedef struct {
    u8 ram[0x10000];
    u8 zram[0x2000];
    u8 sram[MD_SRAM_MAX];
    s32 now;            /* scheduler position within the frame */
    s32 m68k_frac;      /* master clocks not yet a whole CPU cycle */
    s32 z80_frac;
    s32 snd_time;       /* sound is rendered up to here */
    u32 frame;
    u32 buttons[2];
    u32 pad_time[2];
    u16 z80_bank;
    u8 z80_busreq, z80_reset;
    u8 io_reg[16];      /* $A10001..$A1001F */
    u8 pad_state[2];
    u8 six[2];
    u8 region;          /* version register */
    u8 sram_on, sram_writable;
    u8 ssf2;
    u8 bank[8];
    u16 cur_line;       /* scanline the scheduler is on */
    u8 phase;           /* how far through cur_line it has got */
    u16 next_render;    /* next line to draw; may run one past the frame */
    u8 zbank_wait;      /* fraction of a Z80 cycle owed for 68000 bus accesses */
    s32 m68k_debt;      /* master clocks the 68000 owes to Z80 bus accesses */
    s32 ym_busy_until;  /* the YM2612 reports busy until then */
} md_t;

static md_t md;
static u8 rom[MD_ROM_MAX + 0x10000];
static u32 rom_size;        /* rounded up to 64 KiB */
static u32 sram_start, sram_end;
static int sram_present, sram_dirty;
static s16 audio[MD_AUDIO_MAX * 2];
static int audio_count;

/* ---- time ---- */

static inline s32 m68k_time(void) { return m68k.cycles * 7 + md.m68k_frac; }
static inline s32 z80_time(void) { return z80.cycles * 15 + md.z80_frac; }

int md_line_pos(void)
{
    return m68k_time() - (s32)vdp.line * LINE_MCLK;
}

s32 md_now(void)
{
    return m68k_time();
}

void md_dma_stall(u32 mclk)
{
    m68k.cycles += (s32)((mclk + 6) / 7);
}

static void sound_sync(s32 t)
{
    s32 n = (t - md.snd_time) / SAMPLE_MCLK, r;
    if (n <= 0)
        return;
    md.snd_time += n * SAMPLE_MCLK;
    r = n;
    if (audio_count + r > MD_AUDIO_MAX)
        r = MD_AUDIO_MAX - audio_count;
    sound_render(audio + audio_count * 2, r);
    audio_count += r;
}

static void z80_sync(s32 t)
{
    s32 c = (t - md.z80_frac + 14) / 15;
    if (z80.cycles >= c)
        return;
    if (md.z80_reset || md.z80_busreq)
        z80.cycles = c;
    else
        z80_run(c);
}

static void run_to(s32 t)
{
    while (md.now < t) {
        s32 n = md.now + SLICE_MCLK, c;
        if (n > t)
            n = t;
        if (md.m68k_debt) {
            m68k.cycles += md.m68k_debt / 7;
            md.m68k_debt %= 7;
        }
        c = (n - md.m68k_frac + 6) / 7;
        if (m68k.cycles < c)
            m68k_run(c);
        z80_sync(n);
        md.now = n;
    }
}

/* ---- interrupts ---- */

void md_irq_update(void)
{
    int l = 0;
    if (vdp.hint_pending && (vdp.reg[0] & 0x10))
        l = 4;
    if (vdp.vint_pending && (vdp.reg[1] & 0x20))
        l = 6;
    m68k.irq = (u8)l;
}

void m68k_irq_ack(int level)
{
    if (level == 6)
        vdp.vint_pending = 0;
    else if (level == 4)
        vdp.hint_pending = 0;
    md_irq_update();
}

/* ---- memory map ---- */

static void map_setup(void)
{
    u32 p;
    for (p = 0; p < 256; p++)
        m68k_rmap[p] = m68k_wmap[p] = NULL;
    for (p = 0; p < 0x40; p++) {
        u32 off = p << 16;
        if (md.ssf2)
            off = ((u32)md.bank[p >> 3] << 19) | ((p & 7) << 16);
        if (off < rom_size)
            m68k_rmap[p] = rom + off;
    }
    if (sram_present && md.sram_on) {
        for (p = sram_start >> 16; p <= (sram_end >> 16) && p < 0x40; p++)
            m68k_rmap[p] = NULL;
    }
    for (p = 0xE0; p < 0x100; p++)
        m68k_rmap[p] = m68k_wmap[p] = md.ram;
}

u32 md_dma_read16(u32 a)
{
    const u8 *p;
    a &= 0xFFFFFE;
    p = m68k_rmap[a >> 16];
    if (!p)
        return 0xFFFF;
    p += a & 0xFFFF;
    return ((u32)p[0] << 8) | p[1];
}

/* ---- controllers ---- */

static u8 pad_read(int port)
{
    u32 nb = ~md.buttons[port];
    u8 ctrl = md.io_reg[4 + port], data = md.io_reg[1 + port], v;
    int th = (ctrl & 0x40) ? (data & 0x40) : 0x40;
    int state = md.six[port] ? md.pad_state[port] : (th ? 0 : 1);

    switch (state) {
    case 5: v = (u8)((nb >> 2) & 0x30); break;
    case 7: v = (u8)(((nb >> 2) & 0x30) | 0x0F); break;
    case 6: v = (u8)((nb & 0x30) | ((nb >> 8) & 0x0F)); break;
    default:
        v = (state & 1) ? (u8)(((nb >> 2) & 0x30) | (nb & 0x03)) : (u8)(nb & 0x3F);
        break;
    }
    v |= (u8)th;
    return (u8)((data & (ctrl | 0x80)) | (v & ~ctrl & 0x7F));
}

static void pad_write(int port, u8 data, u8 ctrl)
{
    u8 octrl = md.io_reg[4 + port], odata = md.io_reg[1 + port];
    int oth = (octrl & 0x40) ? (odata & 0x40) : 0x40;
    int nth = (ctrl & 0x40) ? (data & 0x40) : 0x40;
    u32 t = md.frame * 313 + vdp.line;

    md.io_reg[1 + port] = data;
    md.io_reg[4 + port] = ctrl;
    if (oth != nth) {
        /* the pad forgets where it was after about 1.5 ms without a strobe */
        if (t - md.pad_time[port] > 24)
            md.pad_state[port] = oth ? 0 : 1;
        md.pad_state[port] = (md.pad_state[port] + 1) & 7;
        md.pad_time[port] = t;
    }
}

static u8 io_read(u32 a)
{
    int r = (a >> 1) & 15;
    switch (r) {
    case 0: return md.region;
    case 1: return pad_read(0);
    case 2: return pad_read(1);
    case 3: return (u8)((md.io_reg[3] & md.io_reg[6]) | (0x7F & ~md.io_reg[6]));
    default: return md.io_reg[r];
    }
}

static void io_write(u32 a, u8 v)
{
    int r = (a >> 1) & 15;
    switch (r) {
    case 0: break;
    case 1: pad_write(0, v, md.io_reg[4]); break;
    case 2: pad_write(1, v, md.io_reg[5]); break;
    case 4: pad_write(0, md.io_reg[1], v); break;
    case 5: pad_write(1, md.io_reg[2], v); break;
    default: md.io_reg[r] = v; break;
    }
}

/* ---- Z80 side ---- */

static void z80_set_busreq(int on)
{
    z80_sync(m68k_time());
    md.z80_busreq = (u8)on;
}

static void z80_set_reset(int on)
{
    z80_sync(m68k_time());
    if (on && !md.z80_reset) {
        z80_reset();
        sound_sync(m68k_time());
        ym_reset();
    }
    md.z80_reset = (u8)on;
}

static u8 zbus_read(u32 a, s32 t)
{
    a &= 0x7FFF;
    if (a < 0x4000)
        return md.zram[a & 0x1FFF];
    if (a < 0x6000) {
        sound_sync(t);
        return (u8)((ym_read() & 0x7F) | (t < md.ym_busy_until ? 0x80 : 0));
    }
    if (a >= 0x7F00 && a < 0x7F20) {
        u16 w;
        switch (a & 0x1C) {
        case 4: w = vdp_ctrl_read(); break;
        case 8: case 12: w = vdp_hv_read(); break;
        default: return 0xFF;
        }
        return (a & 1) ? (u8)w : (u8)(w >> 8);
    }
    return 0xFF;
}

static void zbus_write(u32 a, u8 v, s32 t)
{
    a &= 0x7FFF;
    if (a < 0x4000) {
        md.zram[a & 0x1FFF] = v;
    } else if (a < 0x6000) {
        sound_sync(t);
        ym_write((int)(a & 3), v);
        if (a & 1) /* counted on the chip's own clock, one tick per 42 master clocks */
            md.ym_busy_until = (t + 41) / 42 * 42 + YM_BUSY_MCLK;
    } else if (a < 0x6100) {
        md.z80_bank = (u16)((md.z80_bank >> 1) | ((v & 1) << 8));
    } else if (a >= 0x7F10 && a < 0x7F18) {
        sound_sync(t);
        psg_write(v);
    }
}

static void zbank_access(void)
{
    z80.cycles += 3;
    md.zbank_wait += 2;
    if (md.zbank_wait >= 5) {
        md.zbank_wait -= 5;
        z80.cycles++;
    }
    md.m68k_debt += ZBANK_68K_MCLK;
}

u8 z80_mem_read(u16 a)
{
    u32 full;
    const u8 *p;
    if (a < 0x8000)
        return zbus_read(a, z80_time());
    zbank_access();
    full = ((u32)md.z80_bank << 15) | (a & 0x7FFF);
    p = m68k_rmap[full >> 16];
    if (full < 0xE00000) {
        if (p)
            return p[full & 0xFFFF];
        if (full >= 0xA00000)
            return (u8)m68k_bus_read8(full);
    }
    return 0xFF;
}

void z80_mem_write(u16 a, u8 v)
{
    u32 full;
    if (a < 0x8000) {
        zbus_write(a, v, z80_time());
        return;
    }
    zbank_access();
    full = ((u32)md.z80_bank << 15) | (a & 0x7FFF);
    if (full >= 0xE00000)
        md.ram[full & 0xFFFF] = v;
    else if (full >= 0xA00000)
        m68k_bus_write8(full, v);
}

u8 z80_port_in(u16 port) { (void)port; return 0xFF; }
void z80_port_out(u16 port, u8 v) { (void)port; (void)v; }

/* ---- 68000 side ---- */

static inline int in_sram(u32 a)
{
    return sram_present && md.sram_on && a >= sram_start && a <= sram_end;
}

u32 m68k_bus_read8(u32 a)
{
    if (a < 0x400000) {
        if (in_sram(a))
            return md.sram[(a - sram_start) & (MD_SRAM_MAX - 1)];
        return 0xFF;
    }
    if (a >= 0xA00000 && a < 0xA10000) {
        m68k.cycles++; /* one wait state on the Z80 side of the bus */
        z80_sync(m68k_time());
        return zbus_read(a, m68k_time());
    }
    if (a >= 0xA10000 && a < 0xA10020)
        return io_read(a);
    if ((a & 0xFFFFFE) == 0xA11100)
        return (a & 1) ? 0xFF : (md.z80_busreq ? 0 : 1);
    if (a >= 0xC00000 && a < 0xE00000) {
        u16 w;
        switch (a & 0x1C) {
        case 0: w = vdp_data_read(); break;
        case 4: w = vdp_ctrl_read(); break;
        case 8: case 12: w = vdp_hv_read(); break;
        default: return 0xFF;
        }
        return (a & 1) ? (w & 0xFF) : (u32)(w >> 8);
    }
    return 0xFF;
}

u32 m68k_bus_read16(u32 a)
{
    if (a >= 0xC00000 && a < 0xE00000) {
        switch (a & 0x1C) {
        case 0: return vdp_data_read();
        case 4: return vdp_ctrl_read();
        case 8: case 12: return vdp_hv_read();
        default: return 0xFFFF;
        }
    }
    if (a == 0xA11100)
        return md.z80_busreq ? 0x0000 : 0x0100;
    if (a >= 0xA10000 && a < 0xA10020)
        return io_read(a) | ((u32)io_read(a) << 8);
    return (m68k_bus_read8(a) << 8) | m68k_bus_read8(a + 1);
}

static void cart_ctrl_write(u32 a, u8 v)
{
    if (a == 0xA130F1) {
        md.sram_on = v & 1;
        md.sram_writable = !(v & 2);
        map_setup();
    } else if (a >= 0xA130F3 && a <= 0xA130FF && (a & 1)) {
        md.ssf2 = 1;
        md.bank[(a >> 1) & 7] = v & 0x3F;
        map_setup();
    }
}

void m68k_bus_write8(u32 a, u32 v)
{
    if (a < 0x400000) {
        if (in_sram(a) && md.sram_writable) {
            md.sram[(a - sram_start) & (MD_SRAM_MAX - 1)] = (u8)v;
            sram_dirty = 1;
        }
        return;
    }
    if (a >= 0xA00000 && a < 0xA10000) {
        m68k.cycles++; /* one wait state on the Z80 side of the bus */
        z80_sync(m68k_time());
        zbus_write(a, (u8)v, m68k_time());
        return;
    }
    if (a >= 0xA10000 && a < 0xA10020) {
        if (a & 1)
            io_write(a, (u8)v);
        return;
    }
    if (a == 0xA11100) {
        z80_set_busreq(v & 1);
        return;
    }
    if (a == 0xA11200) {
        z80_set_reset(!(v & 1));
        return;
    }
    if (a >= 0xA13000 && a < 0xA13100) {
        cart_ctrl_write(a, (u8)v);
        return;
    }
    if (a >= 0xC00000 && a < 0xE00000) {
        switch (a & 0x1C) {
        case 0: vdp_data_write((u16)((v << 8) | v)); break;
        case 4: vdp_ctrl_write((u16)((v << 8) | v)); break;
        case 0x10: case 0x14:
            if (a & 1) {
                sound_sync(m68k_time());
                psg_write((u8)v);
            }
            break;
        }
    }
}

void m68k_bus_write16(u32 a, u32 v)
{
    if (a >= 0xC00000 && a < 0xE00000) {
        switch (a & 0x1C) {
        case 0: vdp_data_write((u16)v); break;
        case 4: vdp_ctrl_write((u16)v); break;
        case 0x10: case 0x14:
            sound_sync(m68k_time());
            psg_write((u8)v);
            break;
        }
        return;
    }
    if (a == 0xA11100) {
        z80_set_busreq((v >> 8) & 1);
        return;
    }
    if (a == 0xA11200) {
        z80_set_reset(!((v >> 8) & 1));
        return;
    }
    if (a >= 0xA10000 && a < 0xA10020) {
        io_write(a | 1, (u8)v);
        return;
    }
    m68k_bus_write8(a, v >> 8);
    m68k_bus_write8(a + 1, v & 0xFF);
}

/* ---- frame ---- */

static void frame_wrap(s32 frame_mclk)
{
    s32 t;
    sound_sync(frame_mclk);
    md.snd_time -= frame_mclk;
    md.ym_busy_until -= frame_mclk;
    if (md.ym_busy_until < -frame_mclk)
        md.ym_busy_until = -frame_mclk;
    vdp_frame_end(frame_mclk);
    t = m68k_time() - frame_mclk;
    m68k.refresh_next -= m68k.cycles - t / 7;
    m68k.cycles = t / 7;
    md.m68k_frac = t % 7;
    t = z80_time() - frame_mclk;
    z80.cycles = t / 15;
    md.z80_frac = t % 15;
    md.now -= frame_mclk;
}

enum { PH_START, PH_HINT, PH_VINT, PH_END };

/* run_to, drawing any line whose render point falls before t */
static void advance(s32 t)
{
    int lines = vdp_lines();
    for (;;) {
        int next = md.next_render;
        s32 when = next * LINE_MCLK + RENDER_MCLK;
        if (next >= lines) {
            next -= lines;
            when = (next + lines) * LINE_MCLK + RENDER_MCLK;
        }
        if (when > t)
            break;
        run_to(when);
        if (next < vdp_height())
            vdp_render_line(next);
        md.next_render = (u16)(md.next_render + 1);
    }
    run_to(t);
}

/* Runs one video frame. The call returns just before the vertical interrupt
 * is raised, so the picture is complete and input set before the next call
 * is what the game reads in its V-blank handler. Each scanline goes through:
 * render, H-interrupt counter, V-interrupt (two lines only), end of line. */
void md_run_frame(void)
{
    audio_count = 0;
    for (;;) {
        int lines = vdp_lines(), vdisp = vdp_height(), line = md.cur_line;
        s32 base = line * LINE_MCLK;
        vdp.line = (u16)line;

        if (md.phase == PH_START) {
            if (line == 0) {
                vdp.vblank = 0;
                if (vdp.reg[12] & 2)
                    vdp.odd_frame ^= 1;
                else
                    vdp.odd_frame = 0;
            }
            if (line == vdisp)
                vdp.vblank = 1;
            md.phase = PH_HINT;
        }

        if (md.phase == PH_HINT) {
            /* the H interrupt counter steps on every displayed line and on
             * the line before the display starts; it reloads on the others */
            advance(base + HINT_MCLK);
            if (line < vdisp || line == lines - 1) {
                if (--vdp.hint_counter < 0) {
                    vdp.hint_counter = vdp.reg[10];
                    vdp.hint_pending = 1;
                    md_irq_update();
                }
            } else {
                vdp.hint_counter = vdp.reg[10];
            }
            md.phase = PH_VINT;
        }

        if (md.phase == PH_VINT) {
            /* the V interrupt is raised at the end of the last displayed
             * line; the Z80 sees its INT line held for one line */
            if (line == vdisp - 1) {
                if (md.now < base + VINT_MCLK) {
                    advance(base + VINT_MCLK);
                    sound_sync(md.now);
                    md.frame++;
                    return;
                }
                vdp.vint_pending = 1;
                z80.irq = 1;
                md_irq_update();
            } else if (line == vdisp) {
                advance(base + VINT_MCLK - LINE_MCLK + Z80_INT_MCLK);
                z80.irq = 0;
            }
            md.phase = PH_END;
        }

        advance(base + LINE_MCLK);
        md.phase = PH_START;
        if (++md.cur_line >= lines) {
            md.cur_line = 0;
            md.next_render = (u16)(md.next_render >= lines ? md.next_render - lines : 0);
            frame_wrap(lines * LINE_MCLK);
        }
    }
}

/* ---- setup ---- */

void md_init(int pixel_format)
{
    vdp_abgr = pixel_format == MD_PIXEL_ABGR8888;
    z80_init();
    md.six[0] = md.six[1] = 1;
}

uint8_t *md_rom_buffer(void) { return rom; }

void md_reset(void)
{
    u8 six0 = md.six[0], six1 = md.six[1], region = md.region;
    memset(&md, 0, sizeof md);
    md.six[0] = six0;
    md.six[1] = six1;
    md.region = region;
    md.z80_reset = 1;
    md.sram_writable = 1;
    md.sram_on = sram_present && sram_start >= rom_size;
    md.io_reg[7] = md.io_reg[10] = md.io_reg[13] = 0xFF;
    memset(md.sram, 0xFF, sizeof md.sram);
    { int i; for (i = 0; i < 8; i++) md.bank[i] = (u8)i; }
    map_setup();

    vdp.pal = (region & 0x40) != 0;
    vdp_reset();
    sound_reset();
    memset(&z80, 0, sizeof z80);
    z80_reset();
    memset(&m68k, 0, sizeof m68k);
    m68k.refresh = 1;
    md.cur_line = (u16)(POWER_ON_MCLK / LINE_MCLK);
    md.next_render = (u16)(md.cur_line + 1);
    md.phase = PH_START;
    md.now = md.snd_time = POWER_ON_MCLK;
    m68k.cycles = POWER_ON_MCLK / 7;
    md.m68k_frac = POWER_ON_MCLK % 7;
    z80.cycles = POWER_ON_MCLK / 15;
    md.z80_frac = POWER_ON_MCLK % 15;
    m68k_reset();
    m68k.refresh_next = (m68k.cycles / 128 + 1) * 128;
    audio_count = 0;
    sram_dirty = 0;
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

int md_load(uint32_t size, int region)
{
    u32 i;
    int j = 0, u = 0, e = 0;

    if (size < 0x200 || size > MD_ROM_MAX)
        return 0;

    /* interleaved .smd: 512 byte header, then 16 KiB blocks of odd then even bytes */
    if ((size & 0x3FFF) == 0x200 && rom[8] == 0xAA && rom[9] == 0xBB) {
        static u8 blk[0x4000];
        u32 n = (size - 0x200) / 0x4000, b;
        for (b = 0; b < n; b++) {
            memcpy(blk, rom + 0x200 + b * 0x4000, 0x4000);
            for (i = 0; i < 0x2000; i++) {
                rom[b * 0x4000 + i * 2 + 1] = blk[i];
                rom[b * 0x4000 + i * 2] = blk[0x2000 + i];
            }
        }
        size -= 0x200;
    }

    rom_size = (size + 0xFFFF) & ~0xFFFFu;
    memset(rom + size, 0xFF, rom_size - size);

    /* battery RAM, declared in the header as "RA" plus its address range */
    sram_present = 0;
    if (rom[0x1B0] == 'R' && rom[0x1B1] == 'A') {
        sram_start = be32(rom + 0x1B4) & 0xFFFFFE;
        sram_end = be32(rom + 0x1B8) | 1;
        if (sram_start >= 0x200000 && sram_start < 0x400000 && sram_end > sram_start) {
            if (sram_end - sram_start >= MD_SRAM_MAX)
                sram_end = sram_start + MD_SRAM_MAX - 1;
            sram_present = 1;
        }
    }

    /* region: old headers list letters, newer ones a hex digit of bit flags */
    for (i = 0x1F0; i < 0x1F3; i++) {
        u8 c = rom[i];
        if (c == 'J') j = 1;
        else if (c == 'U') u = 1;
        else if (c == 'E') e = 1;
        else if (i == 0x1F0 && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            int v = c <= '9' ? c - '0' : c - 'A' + 10;
            if (v & 1) j = 1;
            if (v & 4) u = 1;
            if (v & 8) e = 1;
        }
    }
    if (region == MD_REGION_AUTO)
        region = u ? MD_REGION_US : j ? MD_REGION_JP : e ? MD_REGION_EU : MD_REGION_US;
    md.region = region == MD_REGION_JP ? 0x20 : region == MD_REGION_EU ? 0xE0 : 0xA0;

    md_reset();
    return 1;
}

void md_set_input(int port, uint32_t buttons)
{
    if (port == 0 || port == 1)
        md.buttons[port] = buttons;
}

void md_set_audio_filter(int enabled)
{
    sound_lowpass = enabled != 0;
}

void md_set_six_button(int port, int enabled)
{
    if (port == 0 || port == 1)
        md.six[port] = enabled != 0;
}

const uint32_t *md_framebuffer(void) { return vdp_fb; }
int md_width(void) { return vdp_width(); }
int md_height(void) { return vdp_fb_height(); }
int md_is_pal(void) { return vdp.pal; }
uint32_t md_fps_x1000(void) { return vdp.pal ? 49701 : 59923; }
const int16_t *md_audio(void) { return audio; }
int md_audio_frames(void) { return audio_count; }
uint32_t md_audio_rate_x1000(void) { return vdp.pal ? 52781000u : 53267000u; }
uint8_t *md_ram(void) { return md.ram; }
uint8_t *md_sram(void) { return md.sram; }
uint32_t md_sram_size(void) { return sram_present ? sram_end - sram_start + 1 : 0; }

int md_sram_dirty(void)
{
    int d = sram_dirty;
    sram_dirty = 0;
    return d;
}

/* ---- save states ---- */

#define STATE_MAGIC "MDCORE\x01\x00"
#define STATE_SIZE (8 + sizeof(m68k_t) + sizeof(z80_t) + sizeof(vdp_t) + sizeof(ym2612_t) + sizeof(psg_t) + sizeof(md_t))

static u8 state_buf[STATE_SIZE];

uint32_t md_state_size(void) { return (uint32_t)STATE_SIZE; }
uint8_t *md_state_buffer(void) { return state_buf; }

uint32_t md_state_save(uint8_t *out)
{
    u8 *p = out;
    memcpy(p, STATE_MAGIC, 8); p += 8;
    memcpy(p, &m68k, sizeof m68k); p += sizeof m68k;
    memcpy(p, &z80, sizeof z80); p += sizeof z80;
    memcpy(p, &vdp, sizeof vdp); p += sizeof vdp;
    memcpy(p, &ym, sizeof ym); p += sizeof ym;
    memcpy(p, &psg, sizeof psg); p += sizeof psg;
    memcpy(p, &md, sizeof md); p += sizeof md;
    return (uint32_t)(p - out);
}

int md_state_load(const uint8_t *in, uint32_t size)
{
    const u8 *p = in;
    u8 six0 = md.six[0], six1 = md.six[1];
    int i;
    if (size < STATE_SIZE)
        return 0;
    for (i = 0; i < 8; i++)
        if (p[i] != (u8)STATE_MAGIC[i])
            return 0;
    p += 8;
    memcpy(&m68k, p, sizeof m68k); p += sizeof m68k;
    memcpy(&z80, p, sizeof z80); p += sizeof z80;
    memcpy(&vdp, p, sizeof vdp); p += sizeof vdp;
    memcpy(&ym, p, sizeof ym); p += sizeof ym;
    memcpy(&psg, p, sizeof psg); p += sizeof psg;
    memcpy(&md, p, sizeof md);
    md.six[0] = six0;
    md.six[1] = six1;
    md.buttons[0] = md.buttons[1] = 0;
    map_setup();
    vdp_refresh_colors();
    return 1;
}
