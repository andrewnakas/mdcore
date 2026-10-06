/* mdcore - Motorola 68000 interpreter.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
 *
 * Written from the Motorola M68000 Programmer's Reference Manual and User's
 * Manual. Timing is derived from bus activity: every word on the bus costs
 * four clocks, and each instruction adds its documented internal clocks.
 * Address errors are not raised; word accesses ignore bit 0 of the address.
 */
#include "m68k.h"

m68k_t m68k;
u8 *m68k_rmap[256];
u8 *m68k_wmap[256];
#ifdef M68K_TRACE
void (*m68k_trace_hook)(u32 pc);
#endif

#define D m68k.d
#define A m68k.a
#define PC m68k.pc
#define FX m68k.x
#define FN m68k.n
#define FZ m68k.z
#define FV m68k.v
#define FC m68k.c
#define CYC(k) (m68k.cycles += (k))

#define MASK(sz) ((sz) == 1 ? 0xFFu : (sz) == 2 ? 0xFFFFu : 0xFFFFFFFFu)
#define MSB(sz) ((sz) == 1 ? 0x80u : (sz) == 2 ? 0x8000u : 0x80000000u)

/* ---- bus ---- */

static inline u32 rd8(u32 a)
{
    u8 *p;
    a &= 0xFFFFFF;
    CYC(4);
    p = m68k_rmap[a >> 16];
    return p ? p[a & 0xFFFF] : (m68k_bus_read8(a) & 0xFF);
}

static inline u32 rd16(u32 a)
{
    u8 *p;
    a &= 0xFFFFFE;
    CYC(4);
    p = m68k_rmap[a >> 16];
    if (p) {
        p += a & 0xFFFF;
        return ((u32)p[0] << 8) | p[1];
    }
    return m68k_bus_read16(a) & 0xFFFF;
}

static inline u32 rd32(u32 a)
{
    u32 h = rd16(a);
    return (h << 16) | rd16(a + 2);
}

static inline void wr8(u32 a, u32 v)
{
    u8 *p;
    a &= 0xFFFFFF;
    CYC(4);
    p = m68k_wmap[a >> 16];
    if (p)
        p[a & 0xFFFF] = (u8)v;
    else
        m68k_bus_write8(a, v & 0xFF);
}

static inline void wr16(u32 a, u32 v)
{
    u8 *p;
    a &= 0xFFFFFE;
    CYC(4);
    p = m68k_wmap[a >> 16];
    if (p) {
        p += a & 0xFFFF;
        p[0] = (u8)(v >> 8);
        p[1] = (u8)v;
    } else {
        m68k_bus_write16(a, v & 0xFFFF);
    }
}

static inline void wr32(u32 a, u32 v)
{
    wr16(a, v >> 16);
    wr16(a + 2, v & 0xFFFF);
}

static inline u32 rd(u32 a, int sz)
{
    return sz == 1 ? rd8(a) : sz == 2 ? rd16(a) : rd32(a);
}

static inline void wr(u32 a, u32 v, int sz)
{
    if (sz == 1)
        wr8(a, v);
    else if (sz == 2)
        wr16(a, v);
    else
        wr32(a, v);
}

static inline u32 fetch16(void)
{
    u32 v = rd16(PC);
    PC += 2;
    return v;
}

static inline u32 fetch32(void)
{
    u32 h = fetch16();
    return (h << 16) | fetch16();
}

static inline void push32(u32 v)
{
    A[7] -= 4;
    wr32(A[7], v);
}

static inline u32 pop32(void)
{
    u32 v = rd32(A[7]);
    A[7] += 4;
    return v;
}

/* ---- status register ---- */

static inline u32 get_ccr(void)
{
    return (FX << 4) | (FN << 3) | (FZ << 2) | (FV << 1) | FC;
}

static inline void set_ccr(u32 v)
{
    FX = (v >> 4) & 1;
    FN = (v >> 3) & 1;
    FZ = (v >> 2) & 1;
    FV = (v >> 1) & 1;
    FC = v & 1;
}

u32 m68k_get_sr(void)
{
    return ((u32)m68k.t << 15) | ((u32)m68k.s << 13) | ((u32)m68k.imask << 8) | get_ccr();
}

static void set_s(int s)
{
    if (s != m68k.s) {
        u32 t = A[7];
        A[7] = m68k.sp_other;
        m68k.sp_other = t;
        m68k.s = (u8)s;
    }
}

void m68k_set_sr(u32 v)
{
    set_ccr(v);
    set_s((v >> 13) & 1);
    m68k.t = (v >> 15) & 1;
    m68k.imask = (v >> 8) & 7;
}

/* ---- exceptions ---- */

static void exception(int vec, u32 pc)
{
    u32 sr = m68k_get_sr();
    set_s(1);
    m68k.t = 0;
    push32(pc);
    A[7] -= 2;
    wr16(A[7], sr);
    PC = rd32((u32)vec * 4);
}

static void illegal(void)
{
    exception(4, m68k.ir_pc);
    CYC(10);
}

static void privilege(void)
{
    exception(8, m68k.ir_pc);
    CYC(10);
}

/* ---- effective addresses ---- */

static u32 ea_index(u32 base)
{
    u32 ext = fetch16();
    u32 x = (ext & 0x8000) ? A[(ext >> 12) & 7] : D[(ext >> 12) & 7];
    if (!(ext & 0x0800))
        x = (u32)(s32)(s16)x;
    CYC(2);
    return base + x + (u32)(s32)(s8)ext;
}

/* Address of a memory operand. Applies (An)+ and -(An) side effects. */
static u32 ea_addr(int mode, int reg, int sz)
{
    u32 a;
    switch (mode) {
    case 2:
        return A[reg];
    case 3:
        a = A[reg];
        A[reg] += (sz == 1 && reg == 7) ? 2 : (u32)sz;
        return a;
    case 4:
        A[reg] -= (sz == 1 && reg == 7) ? 2 : (u32)sz;
        CYC(2);
        return A[reg];
    case 5:
        a = A[reg];
        return a + (u32)(s32)(s16)fetch16();
    case 6:
        return ea_index(A[reg]);
    case 7:
        switch (reg) {
        case 0:
            return (u32)(s32)(s16)fetch16();
        case 1:
            return fetch32();
        case 2:
            a = PC;
            return a + (u32)(s32)(s16)fetch16();
        case 3:
            a = PC;
            return ea_index(a);
        }
    }
    return 0;
}

static u32 rd_ea(int mode, int reg, int sz)
{
    if (mode == 0)
        return D[reg] & MASK(sz);
    if (mode == 1)
        return A[reg] & MASK(sz);
    if (mode == 7 && reg == 4)
        return sz == 4 ? fetch32() : (fetch16() & MASK(sz));
    return rd(ea_addr(mode, reg, sz), sz);
}

static inline void setd(int r, int sz, u32 v)
{
    if (sz == 4)
        D[r] = v;
    else if (sz == 2)
        D[r] = (D[r] & 0xFFFF0000u) | (v & 0xFFFF);
    else
        D[r] = (D[r] & 0xFFFFFF00u) | (v & 0xFF);
}

/* ---- arithmetic ---- */

static inline void set_nz(u32 r, int sz)
{
    FN = (r & MSB(sz)) != 0;
    FZ = (r & MASK(sz)) == 0;
}

static inline void set_logic(u32 r, int sz)
{
    set_nz(r, sz);
    FV = 0;
    FC = 0;
}

/* d + s (+X). ext selects the ADDX rules for Z. */
static u32 do_add(u32 s, u32 d, int sz, int ext)
{
    u64 r64 = (u64)s + d + (ext ? FX : 0);
    u32 r = (u32)r64 & MASK(sz);
    FC = (u8)((r64 >> (sz * 8)) & 1);
    FX = FC;
    FV = ((s ^ r) & (d ^ r) & MSB(sz)) != 0;
    FN = (r & MSB(sz)) != 0;
    if (ext) {
        if (r)
            FZ = 0;
    } else {
        FZ = r == 0;
    }
    return r;
}

/* d - s (-X). setx is clear for compares. */
static u32 do_sub(u32 s, u32 d, int sz, int ext, int setx)
{
    u64 r64 = (u64)d - s - (ext ? FX : 0);
    u32 r = (u32)r64 & MASK(sz);
    FC = (u8)((r64 >> (sz * 8)) & 1);
    if (setx)
        FX = FC;
    FV = ((s ^ d) & (r ^ d) & MSB(sz)) != 0;
    FN = (r & MSB(sz)) != 0;
    if (ext) {
        if (r)
            FZ = 0;
    } else {
        FZ = r == 0;
    }
    return r;
}

static u32 do_abcd(u32 s, u32 d)
{
    u32 ss = s + d + FX;
    u32 bc = ((s & d) | (~ss & s) | (~ss & d)) & 0x88;
    u32 dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
    u32 corf = (bc | dc) - ((bc | dc) >> 2);
    u32 rr = ss + corf;
    FC = FX = ((bc | (ss & ~rr)) >> 7) & 1;
    FV = ((~ss & rr) >> 7) & 1;
    FN = (rr >> 7) & 1;
    if (rr & 0xFF)
        FZ = 0;
    return rr & 0xFF;
}

static u32 do_sbcd(u32 s, u32 d)
{
    u32 dd = d - s - FX;
    u32 bc = ((~d & s) | (dd & ~d) | (dd & s)) & 0x88;
    u32 corf = bc - (bc >> 2);
    u32 rr = dd - corf;
    FC = FX = ((bc | (~dd & rr)) >> 7) & 1;
    FV = ((dd & ~rr) >> 7) & 1;
    FN = (rr >> 7) & 1;
    if (rr & 0xFF)
        FZ = 0;
    return rr & 0xFF;
}

static int cond(int cc)
{
    switch (cc) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !FC && !FZ;
    case 3: return FC || FZ;
    case 4: return !FC;
    case 5: return FC;
    case 6: return !FZ;
    case 7: return FZ;
    case 8: return !FV;
    case 9: return FV;
    case 10: return !FN;
    case 11: return FN;
    case 12: return FN == FV;
    case 13: return FN != FV;
    case 14: return !FZ && FN == FV;
    default: return FZ || FN != FV;
    }
}

static u32 do_shift(int type, int left, u32 v, u32 cnt, int sz)
{
    u32 msb = MSB(sz), mask = MASK(sz), i, c;
    FV = 0;
    switch (type) {
    case 0: /* arithmetic */
        FC = 0;
        if (left) {
            for (i = 0; i < cnt; i++) {
                FC = (v & msb) != 0;
                v = (v << 1) & mask;
                if (((v & msb) != 0) != FC)
                    FV = 1;
            }
        } else {
            u32 sign = v & msb;
            for (i = 0; i < cnt; i++) {
                FC = v & 1;
                v = (v >> 1) | sign;
            }
        }
        if (cnt)
            FX = FC;
        break;
    case 1: /* logical */
        FC = 0;
        for (i = 0; i < cnt; i++) {
            if (left) {
                FC = (v & msb) != 0;
                v = (v << 1) & mask;
            } else {
                FC = v & 1;
                v >>= 1;
            }
        }
        if (cnt)
            FX = FC;
        break;
    case 2: /* rotate through X */
        for (i = 0; i < cnt; i++) {
            if (left) {
                c = (v & msb) != 0;
                v = ((v << 1) | FX) & mask;
            } else {
                c = v & 1;
                v = (v >> 1) | (FX ? msb : 0);
            }
            FX = (u8)c;
        }
        FC = FX;
        break;
    default: /* rotate */
        FC = 0;
        for (i = 0; i < cnt; i++) {
            if (left) {
                c = (v & msb) != 0;
                v = ((v << 1) | c) & mask;
            } else {
                c = v & 1;
                v = (v >> 1) | (c ? msb : 0);
            }
            FC = (u8)c;
        }
        break;
    }
    set_nz(v, sz);
    return v;
}

static void do_bitop(int type, u32 bit, int mode, int reg)
{
    u32 m, v, a;
    if (mode == 0) {
        bit &= 31;
        m = 1u << bit;
        FZ = !(D[reg] & m);
        switch (type) {
        case 0: CYC(2); break;
        case 1: D[reg] ^= m; CYC(bit < 16 ? 2 : 4); break;
        case 2: D[reg] &= ~m; CYC(bit < 16 ? 4 : 6); break;
        default: D[reg] |= m; CYC(bit < 16 ? 2 : 4); break;
        }
        return;
    }
    m = 1u << (bit & 7);
    if (type == 0) {
        v = rd_ea(mode, reg, 1);
        FZ = !(v & m);
        if (mode == 7 && reg == 4)
            CYC(2);
        return;
    }
    a = ea_addr(mode, reg, 1);
    v = rd8(a);
    FZ = !(v & m);
    if (type == 1)
        v ^= m;
    else if (type == 2)
        v &= ~m;
    else
        v |= m;
    wr8(a, v);
}

static int divu_cycles(u32 dividend, u32 divisor)
{
    int mcycles = 38, i;
    u32 hdivisor = divisor << 16;
    if ((dividend >> 16) >= divisor)
        return 10;
    for (i = 0; i < 15; i++) {
        u32 temp = dividend;
        dividend <<= 1;
        if (temp & 0x80000000u) {
            dividend -= hdivisor;
        } else {
            mcycles += 2;
            if (dividend >= hdivisor) {
                dividend -= hdivisor;
                mcycles--;
            }
        }
    }
    return mcycles * 2;
}

static int divs_cycles(s32 dividend, s32 divisor)
{
    int mcycles = 6, i;
    u32 ad = dividend < 0 ? 0u - (u32)dividend : (u32)dividend;
    u32 as = divisor < 0 ? 0u - (u32)divisor : (u32)divisor;
    u32 aquot;
    if (dividend < 0)
        mcycles++;
    if ((ad >> 16) >= as)
        return (mcycles + 2) * 2;
    aquot = ad / as;
    mcycles += 55;
    if (divisor >= 0) {
        if (dividend >= 0)
            mcycles--;
        else
            mcycles++;
    }
    for (i = 0; i < 15; i++) {
        if ((s16)aquot >= 0)
            mcycles++;
        aquot <<= 1;
    }
    return mcycles * 2;
}

static const s8 jmp_extra[8] = { 2, 0, 2, 4, 0, 0, 0, 0 };

static inline int is_reg_or_imm(int mode, int reg)
{
    return mode <= 1 || (mode == 7 && reg == 4);
}

/* Shared body of the 8/9/B/C/D lines: kind 0 OR, 1 AND, 2 SUB, 3 ADD, 4 EOR, 5 CMP. */
static u32 alu(int kind, u32 s, u32 d, int sz)
{
    u32 r;
    switch (kind) {
    case 0: r = s | d; set_logic(r, sz); return r;
    case 1: r = s & d; set_logic(r, sz); return r;
    case 2: return do_sub(s, d, sz, 0, 1);
    case 3: return do_add(s, d, sz, 0);
    case 4: r = s ^ d; set_logic(r, sz); return r;
    default: do_sub(s, d, sz, 0, 0); return d;
    }
}

static void step(void)
{
    u32 op, a, s, d, r;
    int mode, reg, r2, sz;

    m68k.ir_pc = PC;
#ifdef M68K_TRACE
    if (m68k_trace_hook)
        m68k_trace_hook(PC);
#endif
    op = fetch16();
    mode = (op >> 3) & 7;
    reg = op & 7;
    r2 = (op >> 9) & 7;

    switch (op >> 12) {
    case 0x0:
        if (op & 0x100) {
            if (mode == 1) { /* MOVEP */
                a = A[reg];
                a += (u32)(s32)(s16)fetch16();
                switch ((op >> 6) & 3) {
                case 0:
                    r = rd8(a) << 8;
                    r |= rd8(a + 2);
                    setd(r2, 2, r);
                    break;
                case 1:
                    r = rd8(a) << 24;
                    r |= rd8(a + 2) << 16;
                    r |= rd8(a + 4) << 8;
                    r |= rd8(a + 6);
                    D[r2] = r;
                    break;
                case 2:
                    wr8(a, D[r2] >> 8);
                    wr8(a + 2, D[r2]);
                    break;
                default:
                    wr8(a, D[r2] >> 24);
                    wr8(a + 2, D[r2] >> 16);
                    wr8(a + 4, D[r2] >> 8);
                    wr8(a + 6, D[r2]);
                    break;
                }
            } else {
                do_bitop((op >> 6) & 3, D[r2], mode, reg);
            }
            break;
        }
        if (r2 == 4) { /* static bit operations */
            s = fetch16();
            do_bitop((op >> 6) & 3, s, mode, reg);
            break;
        }
        if (r2 == 7 || ((op >> 6) & 3) == 3) {
            illegal();
            break;
        }
        if ((op & 0xBF) == 0x3C && (r2 == 0 || r2 == 1 || r2 == 5)) { /* to CCR / SR */
            int to_sr = (op & 0x40) != 0;
            if (to_sr && !m68k.s) {
                privilege();
                break;
            }
            s = fetch16();
            d = m68k_get_sr();
            if (!to_sr)
                s &= 0xFF;
            if (r2 == 0)
                r = d | s;
            else if (r2 == 1)
                r = d & (to_sr ? s : (s | 0xFF00));
            else
                r = d ^ s;
            if (to_sr)
                m68k_set_sr(r & 0xA71F);
            else
                set_ccr(r);
            CYC(12);
            break;
        }
        sz = 1 << ((op >> 6) & 3);
        s = sz == 4 ? fetch32() : (fetch16() & MASK(sz));
        {
            static const u8 kinds[8] = { 0, 1, 2, 3, 0, 4, 5, 0 };
            int kind = kinds[r2];
            if (mode == 0) {
                d = D[reg] & MASK(sz);
                r = alu(kind, s, d, sz);
                if (kind != 5)
                    setd(reg, sz, r);
                if (sz == 4)
                    CYC(kind == 5 ? 2 : 4);
            } else {
                a = ea_addr(mode, reg, sz);
                d = rd(a, sz);
                r = alu(kind, s, d, sz);
                if (kind != 5)
                    wr(a, r, sz);
            }
        }
        break;

    case 0x1:
    case 0x2:
    case 0x3: {
        int dmode = (op >> 6) & 7;
        sz = (op >> 12) == 1 ? 1 : (op >> 12) == 3 ? 2 : 4;
        r = rd_ea(mode, reg, sz);
        if (dmode == 1) { /* MOVEA */
            A[r2] = sz == 2 ? (u32)(s32)(s16)r : r;
            break;
        }
        set_logic(r, sz);
        if (dmode == 0) {
            setd(r2, sz, r);
        } else {
            if (dmode == 4)
                CYC(-2);
            a = ea_addr(dmode, r2, sz);
            wr(a, r, sz);
        }
        break;
    }

    case 0x4:
        if (op & 0x100) {
            if ((op & 0xC0) == 0xC0) { /* LEA */
                A[r2] = ea_addr(mode, reg, 0);
                if (mode == 6 || (mode == 7 && reg == 3))
                    CYC(2);
            } else if ((op & 0xC0) == 0x80) { /* CHK */
                s32 bound = (s16)rd_ea(mode, reg, 2);
                s32 v = (s16)D[r2];
                FZ = (v & 0xFFFF) == 0;
                FV = 0;
                FC = 0;
                if (v < 0 || v > bound) {
                    FN = v < 0;
                    exception(6, PC);
                    /* the bound compare is made first and ignores overflow */
                    CYC(v > bound || (((v - bound) & 0x8000) == 0 && v != bound) ? 14 : 16);
                } else {
                    FN = 0;
                    CYC(6);
                }
            } else {
                illegal();
            }
            break;
        }
        sz = 1 << ((op >> 6) & 3);
        switch ((op >> 9) & 7) {
        case 0:
            if (sz == 8) { /* MOVE from SR */
                if (mode == 0) {
                    setd(reg, 2, m68k_get_sr());
                    CYC(2);
                } else {
                    a = ea_addr(mode, reg, 2);
                    rd16(a);
                    wr16(a, m68k_get_sr());
                }
                break;
            }
            if (mode == 0) { /* NEGX */
                r = do_sub(D[reg] & MASK(sz), 0, sz, 1, 1);
                setd(reg, sz, r);
                if (sz == 4)
                    CYC(2);
            } else {
                a = ea_addr(mode, reg, sz);
                r = do_sub(rd(a, sz), 0, sz, 1, 1);
                wr(a, r, sz);
            }
            break;
        case 1: /* CLR */
            if (sz == 8) {
                illegal();
                break;
            }
            if (mode == 0) {
                setd(reg, sz, 0);
                if (sz == 4)
                    CYC(2);
            } else {
                a = ea_addr(mode, reg, sz);
                rd(a, sz);
                wr(a, 0, sz);
            }
            FN = 0;
            FZ = 1;
            FV = 0;
            FC = 0;
            break;
        case 2:
            if (sz == 8) { /* MOVE to CCR */
                set_ccr(rd_ea(mode, reg, 2));
                CYC(8);
                break;
            }
            if (mode == 0) { /* NEG */
                r = do_sub(D[reg] & MASK(sz), 0, sz, 0, 1);
                setd(reg, sz, r);
                if (sz == 4)
                    CYC(2);
            } else {
                a = ea_addr(mode, reg, sz);
                r = do_sub(rd(a, sz), 0, sz, 0, 1);
                wr(a, r, sz);
            }
            break;
        case 3:
            if (sz == 8) { /* MOVE to SR */
                if (!m68k.s) {
                    privilege();
                    break;
                }
                m68k_set_sr(rd_ea(mode, reg, 2) & 0xA71F);
                CYC(8);
                break;
            }
            if (mode == 0) { /* NOT */
                r = ~D[reg] & MASK(sz);
                setd(reg, sz, r);
                if (sz == 4)
                    CYC(2);
            } else {
                a = ea_addr(mode, reg, sz);
                r = ~rd(a, sz) & MASK(sz);
                wr(a, r, sz);
            }
            set_logic(r, sz);
            break;
        case 4:
            if (sz == 1) { /* NBCD */
                if (mode == 0) {
                    setd(reg, 1, do_sbcd(D[reg] & 0xFF, 0));
                    CYC(2);
                } else {
                    a = ea_addr(mode, reg, 1);
                    r = do_sbcd(rd8(a), 0);
                    wr8(a, r);
                }
            } else if (sz == 2) {
                if (mode == 0) { /* SWAP */
                    D[reg] = (D[reg] << 16) | (D[reg] >> 16);
                    set_logic(D[reg], 4);
                } else { /* PEA */
                    a = ea_addr(mode, reg, 0);
                    if (mode == 6 || (mode == 7 && reg == 3))
                        CYC(2);
                    push32(a);
                }
            } else if (mode == 0) { /* EXT */
                if (sz == 4) {
                    setd(reg, 2, (u32)(s32)(s8)D[reg]);
                    set_logic(D[reg], 2);
                } else {
                    D[reg] = (u32)(s32)(s16)D[reg];
                    set_logic(D[reg], 4);
                }
            } else { /* MOVEM registers to memory */
                u32 list = fetch16();
                int i, msz = sz == 4 ? 2 : 4;
                if (mode == 4) {
                    a = A[reg];
                    for (i = 0; i < 16; i++) {
                        if (list & (1u << i)) {
                            a -= (u32)msz;
                            wr(a, i < 8 ? A[7 - i] : D[15 - i], msz);
                        }
                    }
                    A[reg] = a;
                } else {
                    a = ea_addr(mode, reg, 0);
                    for (i = 0; i < 16; i++) {
                        if (list & (1u << i)) {
                            wr(a, i < 8 ? D[i] : A[i - 8], msz);
                            a += (u32)msz;
                        }
                    }
                }
            }
            break;
        case 5:
            if (sz == 8) {
                if (op == 0x4AFC) {
                    illegal();
                    break;
                }
                /* TAS */
                if (mode == 0) {
                    r = D[reg] & 0xFF;
                    set_logic(r, 1);
                    setd(reg, 1, r | 0x80);
                } else {
                    a = ea_addr(mode, reg, 1);
                    r = rd8(a);
                    set_logic(r, 1);
                    if (m68k.tas_writes)
                        wr8(a, r | 0x80);
                    else
                        CYC(4);
                    CYC(2);
                }
                break;
            }
            r = rd_ea(mode, reg, sz); /* TST */
            set_logic(r, sz);
            break;
        case 6:
            if (sz < 4) {
                illegal();
            } else { /* MOVEM memory to registers */
                u32 list = fetch16();
                int i, msz = sz == 4 ? 2 : 4;
                a = mode == 3 ? A[reg] : ea_addr(mode, reg, 0);
                for (i = 0; i < 16; i++) {
                    if (list & (1u << i)) {
                        r = rd(a, msz);
                        if (msz == 2)
                            r = (u32)(s32)(s16)r;
                        if (i < 8)
                            D[i] = r;
                        else
                            A[i - 8] = r;
                        a += (u32)msz;
                    }
                }
                CYC(4); /* the 68000 reads one word past the list */
                if (mode == 3)
                    A[reg] = a;
            }
            break;
        default:
            if (op & 0x80) { /* JSR / JMP */
                a = ea_addr(mode, reg, 0);
                CYC(mode == 2 ? 4 : mode == 5 ? 2 : mode == 6 ? 4 : jmp_extra[reg]);
                if (!(op & 0x40))
                    push32(PC);
                PC = a;
                break;
            }
            if (!(op & 0x40)) {
                illegal();
                break;
            }
            switch (mode) {
            case 0:
            case 1: /* TRAP */
                exception(32 + (int)(op & 15), PC);
                CYC(10);
                break;
            case 2: /* LINK */
                s = fetch16();
                r = A[reg];
                A[7] -= 4;
                wr32(A[7], r);
                A[reg] = A[7];
                A[7] += (u32)(s32)(s16)s;
                break;
            case 3: /* UNLK */
                A[7] = A[reg];
                r = pop32();
                A[reg] = r;
                break;
            case 4: /* MOVE An,USP */
                if (!m68k.s)
                    privilege();
                else
                    m68k.sp_other = A[reg];
                break;
            case 5: /* MOVE USP,An */
                if (!m68k.s)
                    privilege();
                else
                    A[reg] = m68k.sp_other;
                break;
            case 6:
                switch (reg) {
                case 0: /* RESET */
                    if (!m68k.s)
                        privilege();
                    else
                        CYC(128);
                    break;
                case 1: /* NOP */
                    break;
                case 2: /* STOP */
                    if (!m68k.s) {
                        privilege();
                    } else {
                        m68k_set_sr(fetch16() & 0xA71F);
                        m68k.stopped = 1;
                        CYC(-4);
                    }
                    break;
                case 3: /* RTE */
                    if (!m68k.s) {
                        privilege();
                    } else {
                        s = rd16(A[7]);
                        A[7] += 2;
                        PC = pop32();
                        m68k_set_sr(s & 0xA71F);
                        CYC(4);
                    }
                    break;
                case 5: /* RTS */
                    PC = pop32();
                    CYC(4);
                    break;
                case 6: /* TRAPV */
                    if (FV) {
                        exception(7, PC);
                        CYC(10);
                    }
                    break;
                case 7: /* RTR */
                    s = rd16(A[7]);
                    A[7] += 2;
                    PC = pop32();
                    set_ccr(s);
                    CYC(4);
                    break;
                default:
                    illegal();
                    break;
                }
                break;
            default:
                illegal();
                break;
            }
            break;
        }
        break;

    case 0x5:
        if ((op & 0xC0) == 0xC0) {
            int cc = (op >> 8) & 15;
            if (mode == 1) { /* DBcc */
                u32 base = PC;
                s = fetch16();
                if (cond(cc)) {
                    CYC(4);
                } else {
                    r = (D[reg] - 1) & 0xFFFF;
                    setd(reg, 2, r);
                    if (r != 0xFFFF) {
                        PC = base + (u32)(s32)(s16)s;
                        CYC(2);
                    } else {
                        CYC(6);
                    }
                }
            } else { /* Scc */
                int t = cond(cc);
                if (mode == 0) {
                    setd(reg, 1, t ? 0xFF : 0);
                    if (t)
                        CYC(2);
                } else {
                    a = ea_addr(mode, reg, 1);
                    rd8(a);
                    wr8(a, t ? 0xFF : 0);
                }
            }
            break;
        }
        s = r2 ? (u32)r2 : 8; /* ADDQ / SUBQ */
        sz = 1 << ((op >> 6) & 3);
        if (mode == 1) {
            if (op & 0x100)
                A[reg] -= s;
            else
                A[reg] += s;
            CYC(4);
        } else if (mode == 0) {
            d = D[reg] & MASK(sz);
            r = (op & 0x100) ? do_sub(s, d, sz, 0, 1) : do_add(s, d, sz, 0);
            setd(reg, sz, r);
            if (sz == 4)
                CYC(4);
        } else {
            a = ea_addr(mode, reg, sz);
            d = rd(a, sz);
            r = (op & 0x100) ? do_sub(s, d, sz, 0, 1) : do_add(s, d, sz, 0);
            wr(a, r, sz);
        }
        break;

    case 0x6: {
        int cc = (op >> 8) & 15, wide = 0;
        u32 base = PC;
        s32 disp = (s8)op;
        if (disp == 0) {
            disp = (s16)fetch16();
            wide = 1;
        }
        if (cc == 1) { /* BSR */
            push32(PC);
            PC = base + (u32)disp;
            CYC(wide ? 2 : 6);
        } else if (cond(cc)) {
            PC = base + (u32)disp;
            CYC(wide ? 2 : 6);
        } else {
            CYC(4);
        }
        break;
    }

    case 0x7: /* MOVEQ */
        if (op & 0x100) {
            illegal();
            break;
        }
        D[r2] = (u32)(s32)(s8)op;
        set_logic(D[r2], 4);
        break;

    case 0x8:
    case 0x9:
    case 0xB:
    case 0xC:
    case 0xD: {
        int line = (int)(op >> 12);
        int opmode = (op >> 6) & 7;
        int kind = line == 0x8 ? 0 : line == 0xC ? 1 : line == 0x9 ? 2 : line == 0xD ? 3 : 5;

        if (opmode == 3 || opmode == 7) {
            if (line == 0x8) { /* DIVU / DIVS */
                s = rd_ea(mode, reg, 2);
                if (s == 0) {
                    FN = 0;
                    FZ = 0;
                    FV = 0;
                    FC = 0;
                    exception(5, PC);
                    CYC(14);
                    break;
                }
                FC = 0;
                if (opmode == 3) {
                    u32 q;
                    d = D[r2];
                    CYC(divu_cycles(d, s) - 4);
                    q = d / s;
                    if (q > 0xFFFF) {
                        FV = 1;
                        FN = 1;
                        FZ = 0;
                    } else {
                        D[r2] = ((d % s) << 16) | q;
                        FV = 0;
                        FN = (q >> 15) & 1;
                        FZ = q == 0;
                    }
                } else {
                    s32 sd = (s32)D[r2], ss = (s16)s, q, rem;
                    CYC(divs_cycles(sd, ss) - 4);
                    if (sd == (s32)0x80000000 && ss == -1) {
                        FV = 1;
                        FN = 1;
                        FZ = 0;
                        break;
                    }
                    q = sd / ss;
                    rem = sd % ss;
                    if (q != (s16)q) {
                        FV = 1;
                        FN = 1;
                        FZ = 0;
                    } else {
                        D[r2] = (((u32)rem & 0xFFFF) << 16) | ((u32)q & 0xFFFF);
                        FV = 0;
                        FN = ((u32)q >> 15) & 1;
                        FZ = q == 0;
                    }
                }
            } else if (line == 0xC) { /* MULU / MULS */
                u32 bits;
                int n = 0;
                s = rd_ea(mode, reg, 2);
                if (opmode == 3) {
                    r = (D[r2] & 0xFFFF) * s;
                    bits = s;
                } else {
                    r = (u32)((s32)(s16)D[r2] * (s32)(s16)s);
                    bits = ((s << 1) ^ s) & 0xFFFF;
                }
                for (; bits; bits >>= 1)
                    n += (int)(bits & 1);
                D[r2] = r;
                set_logic(r, 4);
                CYC(34 + 2 * n);
            } else { /* ADDA / SUBA / CMPA */
                sz = opmode == 3 ? 2 : 4;
                s = rd_ea(mode, reg, sz);
                if (sz == 2)
                    s = (u32)(s32)(s16)s;
                if (line == 0xB) {
                    do_sub(s, A[r2], 4, 0, 0);
                    CYC(2);
                } else {
                    if (line == 0x9)
                        A[r2] -= s;
                    else
                        A[r2] += s;
                    CYC(sz == 2 || is_reg_or_imm(mode, reg) ? 4 : 2);
                }
            }
            break;
        }

        sz = 1 << (opmode & 3);
        if (opmode < 3) { /* <ea> op Dn -> Dn */
            s = rd_ea(mode, reg, sz);
            d = D[r2] & MASK(sz);
            r = alu(kind, s, d, sz);
            if (kind != 5)
                setd(r2, sz, r);
            if (sz == 4)
                CYC(kind == 5 ? 2 : is_reg_or_imm(mode, reg) ? 4 : 2);
            break;
        }

        /* opmode 4..6 */
        if (line == 0xB) {
            if (mode == 1) { /* CMPM */
                s = rd(ea_addr(3, reg, sz), sz);
                d = rd(ea_addr(3, r2, sz), sz);
                do_sub(s, d, sz, 0, 0);
            } else if (mode == 0) { /* EOR Dn,Dn */
                r = (D[reg] ^ D[r2]) & MASK(sz);
                set_logic(r, sz);
                setd(reg, sz, r);
                if (sz == 4)
                    CYC(4);
            } else {
                a = ea_addr(mode, reg, sz);
                r = (rd(a, sz) ^ D[r2]) & MASK(sz);
                set_logic(r, sz);
                wr(a, r, sz);
            }
            break;
        }
        if (mode <= 1) {
            if (line == 0xC && opmode != 4) { /* EXG */
                u32 t;
                if (opmode == 5 && mode == 0) {
                    t = D[r2]; D[r2] = D[reg]; D[reg] = t;
                } else if (opmode == 5) {
                    t = A[r2]; A[r2] = A[reg]; A[reg] = t;
                } else if (mode == 1) {
                    t = D[r2]; D[r2] = A[reg]; A[reg] = t;
                } else {
                    illegal();
                    break;
                }
                CYC(2);
                break;
            }
            if ((line == 0x8 || line == 0xC) && opmode != 4) {
                illegal();
                break;
            }
            /* ADDX / SUBX / ABCD / SBCD */
            if (mode == 0) {
                s = D[reg] & MASK(sz);
                d = D[r2] & MASK(sz);
            } else {
                A[reg] -= (sz == 1 && reg == 7) ? 2 : (u32)sz;
                s = rd(A[reg], sz);
                A[r2] -= (sz == 1 && r2 == 7) ? 2 : (u32)sz;
                d = rd(A[r2], sz);
            }
            if (line == 0x8)
                r = do_sbcd(s, d);
            else if (line == 0xC)
                r = do_abcd(s, d);
            else if (line == 0x9)
                r = do_sub(s, d, sz, 1, 1);
            else
                r = do_add(s, d, sz, 1);
            if (mode == 0) {
                setd(r2, sz, r);
                if (line == 0x8 || line == 0xC)
                    CYC(2);
                else if (sz == 4)
                    CYC(4);
            } else {
                wr(A[r2], r, sz);
                CYC(2);
            }
            break;
        }
        a = ea_addr(mode, reg, sz); /* Dn op <ea> -> <ea> */
        d = rd(a, sz);
        r = alu(kind, D[r2] & MASK(sz), d, sz);
        wr(a, r, sz);
        break;
    }

    case 0xA:
        exception(10, m68k.ir_pc);
        CYC(10);
        break;

    case 0xE: {
        int left = (op >> 8) & 1;
        if (((op >> 6) & 3) == 3) {
            if (op & 0x800) {
                illegal();
                break;
            }
            a = ea_addr(mode, reg, 2);
            r = do_shift((op >> 9) & 3, left, rd16(a), 1, 2);
            wr16(a, r);
        } else {
            u32 cnt = (op & 0x20) ? (D[r2] & 63) : (r2 ? (u32)r2 : 8);
            sz = 1 << ((op >> 6) & 3);
            r = do_shift((op >> 3) & 3, left, D[reg] & MASK(sz), cnt, sz);
            setd(reg, sz, r);
            CYC((sz == 4 ? 4 : 2) + 2 * (int)cnt);
        }
        break;
    }

    default: /* line F */
        exception(11, m68k.ir_pc);
        CYC(10);
        break;
    }
}

void m68k_reset(void)
{
    m68k.s = 0;
    set_s(1);
    m68k.t = 0;
    m68k.imask = 7;
    m68k.stopped = 0;
    m68k.irq = 0;
    A[7] = rd32(0);
    PC = rd32(4);
}

void m68k_run(s32 until)
{
    while (m68k.cycles < until) {
        if (m68k.irq > m68k.imask || m68k.irq == 7) {
            int level = m68k.irq;
            m68k.stopped = 0;
            exception(24 + level, PC);
            m68k.imask = (u8)level;
            CYC(24);
            m68k_irq_ack(level);
            continue;
        }
        if (m68k.stopped) {
            m68k.cycles = until;
            break;
        }
        /* The console's DRAM refresh holds the 68000 off the bus for about
         * two clocks in every 128. */
        if (m68k.refresh && m68k.cycles >= m68k.refresh_next) {
            m68k.refresh_next = m68k.cycles + 128;
            CYC(2);
        }
        step();
    }
}
