/* mdcore - Zilog Z80 interpreter.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
 *
 * Written from the Zilog Z80 CPU User Manual and the public descriptions of
 * the undocumented opcodes and flag bits. Instructions are decoded by the
 * usual x/y/z bit fields of the opcode.
 */
#include "z80.h"

z80_t z80;
#ifdef Z80_TRACE
void (*z80_trace_hook)(u16 pc);
#endif

#define FLAG_C 0x01
#define FLAG_N 0x02
#define FLAG_PV 0x04
#define FLAG_X 0x08
#define FLAG_H 0x10
#define FLAG_Y 0x20
#define FLAG_Z 0x40
#define FLAG_S 0x80

#define Z z80
#define CYC(k) (z80.cycles += (k))

static u8 szp[256];     /* sign, zero, bits 5 and 3, parity */
static u16 *hlp;        /* HL, IX or IY for the instruction being decoded */

#define rb(a) z80_mem_read((u16)(a))
#define wb(a, v) z80_mem_write((u16)(a), (u8)(v))

static inline u8 fetch(void)
{
    return rb(Z.pc++);
}

static inline u16 fetch16(void)
{
    u16 lo = fetch();
    return (u16)(lo | (fetch() << 8));
}

static inline u16 rw(u16 a)
{
    u16 lo = rb(a);
    return (u16)(lo | (rb(a + 1) << 8));
}

static inline void ww(u16 a, u16 v)
{
    wb(a, v);
    wb(a + 1, v >> 8);
}

static inline void push(u16 v)
{
    Z.sp--;
    wb(Z.sp, v >> 8);
    Z.sp--;
    wb(Z.sp, v);
}

static inline u16 pop(void)
{
    u16 v = rw(Z.sp);
    Z.sp += 2;
    return v;
}

/* ---- 8-bit register access ---- */

static u8 getr0(int r)
{
    switch (r) {
    case 0: return Z.bc >> 8;
    case 1: return (u8)Z.bc;
    case 2: return Z.de >> 8;
    case 3: return (u8)Z.de;
    case 4: return Z.hl >> 8;
    case 5: return (u8)Z.hl;
    default: return Z.a;
    }
}

static void setr0(int r, u8 v)
{
    switch (r) {
    case 0: Z.bc = (u16)((Z.bc & 0x00FF) | (v << 8)); break;
    case 1: Z.bc = (u16)((Z.bc & 0xFF00) | v); break;
    case 2: Z.de = (u16)((Z.de & 0x00FF) | (v << 8)); break;
    case 3: Z.de = (u16)((Z.de & 0xFF00) | v); break;
    case 4: Z.hl = (u16)((Z.hl & 0x00FF) | (v << 8)); break;
    case 5: Z.hl = (u16)((Z.hl & 0xFF00) | v); break;
    default: Z.a = v; break;
    }
}

/* As above, but H and L follow a DD/FD prefix. */
static u8 getr(int r)
{
    if (r == 4)
        return *hlp >> 8;
    if (r == 5)
        return (u8)*hlp;
    return getr0(r);
}

static void setr(int r, u8 v)
{
    if (r == 4)
        *hlp = (u16)((*hlp & 0x00FF) | (v << 8));
    else if (r == 5)
        *hlp = (u16)((*hlp & 0xFF00) | v);
    else
        setr0(r, v);
}

/* Address of the (HL) operand; fetches the displacement under a prefix. */
static u16 hl_addr(void)
{
    if (hlp == &Z.hl)
        return Z.hl;
    CYC(8);
    return (u16)(*hlp + (s8)fetch());
}

static u16 *rp(int p)
{
    switch (p) {
    case 0: return &Z.bc;
    case 1: return &Z.de;
    case 2: return hlp;
    default: return &Z.sp;
    }
}

/* ---- arithmetic ---- */

static void add8(u8 v, int c)
{
    unsigned r = Z.a + v + (unsigned)c;
    Z.f = (u8)((szp[r & 0xFF] & ~FLAG_PV) | ((Z.a ^ v ^ r) & FLAG_H) |
               (((Z.a ^ r) & (v ^ r) & 0x80) >> 5) | (r >> 8));
    Z.a = (u8)r;
}

static void sub8(u8 v, int c)
{
    unsigned r = Z.a - v - (unsigned)c;
    Z.f = (u8)((szp[r & 0xFF] & ~FLAG_PV) | FLAG_N | ((Z.a ^ v ^ r) & FLAG_H) |
               (((Z.a ^ v) & (Z.a ^ r) & 0x80) >> 5) | ((r >> 8) & 1));
    Z.a = (u8)r;
}

static void cp8(u8 v)
{
    unsigned r = Z.a - v;
    Z.f = (u8)((szp[r & 0xFF] & ~(FLAG_PV | FLAG_X | FLAG_Y)) | (v & (FLAG_X | FLAG_Y)) | FLAG_N |
               ((Z.a ^ v ^ r) & FLAG_H) | (((Z.a ^ v) & (Z.a ^ r) & 0x80) >> 5) | ((r >> 8) & 1));
}

static void alu(int op, u8 v)
{
    switch (op) {
    case 0: add8(v, 0); break;
    case 1: add8(v, Z.f & FLAG_C); break;
    case 2: sub8(v, 0); break;
    case 3: sub8(v, Z.f & FLAG_C); break;
    case 4: Z.a &= v; Z.f = szp[Z.a] | FLAG_H; break;
    case 5: Z.a ^= v; Z.f = szp[Z.a]; break;
    case 6: Z.a |= v; Z.f = szp[Z.a]; break;
    default: cp8(v); break;
    }
}

static u8 inc8(u8 v)
{
    v++;
    Z.f = (u8)((Z.f & FLAG_C) | (szp[v] & ~FLAG_PV) | ((v & 0x0F) ? 0 : FLAG_H) | (v == 0x80 ? FLAG_PV : 0));
    return v;
}

static u8 dec8(u8 v)
{
    v--;
    Z.f = (u8)((Z.f & FLAG_C) | FLAG_N | (szp[v] & ~FLAG_PV) | ((v & 0x0F) == 0x0F ? FLAG_H : 0) |
               (v == 0x7F ? FLAG_PV : 0));
    return v;
}

static u8 rot(int op, u8 v)
{
    u8 c;
    switch (op) {
    case 0: c = v >> 7; v = (u8)((v << 1) | c); break;                        /* RLC */
    case 1: c = v & 1; v = (u8)((v >> 1) | (c << 7)); break;                  /* RRC */
    case 2: c = v >> 7; v = (u8)((v << 1) | (Z.f & FLAG_C)); break;           /* RL */
    case 3: c = v & 1; v = (u8)((v >> 1) | ((Z.f & FLAG_C) << 7)); break;     /* RR */
    case 4: c = v >> 7; v = (u8)(v << 1); break;                              /* SLA */
    case 5: c = v & 1; v = (u8)((v >> 1) | (v & 0x80)); break;                /* SRA */
    case 6: c = v >> 7; v = (u8)((v << 1) | 1); break;                        /* SLL */
    default: c = v & 1; v >>= 1; break;                                       /* SRL */
    }
    Z.f = szp[v] | c;
    return v;
}

static void add16(u16 *dst, u16 v)
{
    unsigned r = *dst + v;
    Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_PV)) | (((*dst ^ v ^ r) >> 8) & FLAG_H) |
               ((r >> 8) & (FLAG_X | FLAG_Y)) | (r >> 16));
    *dst = (u16)r;
}

static void adc16(u16 v)
{
    unsigned r = Z.hl + v + (Z.f & FLAG_C);
    Z.f = (u8)(((r >> 8) & (FLAG_S | FLAG_X | FLAG_Y)) | ((r & 0xFFFF) ? 0 : FLAG_Z) |
               (((Z.hl ^ v ^ r) >> 8) & FLAG_H) | (((~(Z.hl ^ v) & (Z.hl ^ r)) >> 13) & FLAG_PV) | (r >> 16));
    Z.hl = (u16)r;
}

static void sbc16(u16 v)
{
    unsigned r = Z.hl - v - (Z.f & FLAG_C);
    Z.f = (u8)(((r >> 8) & (FLAG_S | FLAG_X | FLAG_Y)) | ((r & 0xFFFF) ? 0 : FLAG_Z) | FLAG_N |
               (((Z.hl ^ v ^ r) >> 8) & FLAG_H) | ((((Z.hl ^ v) & (Z.hl ^ r)) >> 13) & FLAG_PV) | ((r >> 16) & 1));
    Z.hl = (u16)r;
}

static int cond(int cc)
{
    switch (cc) {
    case 0: return !(Z.f & FLAG_Z);
    case 1: return Z.f & FLAG_Z;
    case 2: return !(Z.f & FLAG_C);
    case 3: return Z.f & FLAG_C;
    case 4: return !(Z.f & FLAG_PV);
    case 5: return Z.f & FLAG_PV;
    case 6: return !(Z.f & FLAG_S);
    default: return Z.f & FLAG_S;
    }
}

static void daa(void)
{
    u8 a = Z.a, t = 0, c = Z.f & FLAG_C, h;
    if ((Z.f & FLAG_H) || (a & 0x0F) > 9)
        t |= 0x06;
    if (c || a > 0x99) {
        t |= 0x60;
        c = 1;
    }
    if (Z.f & FLAG_N) {
        h = (Z.f & FLAG_H) && (a & 0x0F) < 6;
        a -= t;
    } else {
        h = (a & 0x0F) > 9;
        a += t;
    }
    Z.a = a;
    Z.f = (u8)(szp[a] | c | (Z.f & FLAG_N) | (h ? FLAG_H : 0));
}

/* ---- prefixed groups ---- */

static void exec_cb(void)
{
    u8 op, v;
    u16 a = 0;
    int x, y, zz, mem;

    if (hlp != &Z.hl) {
        a = (u16)(*hlp + (s8)fetch());
        op = fetch();
        mem = 1;
        CYC(4);
    } else {
        op = fetch();
        Z.r = (Z.r & 0x80) | ((Z.r + 1) & 0x7F);
        mem = (op & 7) == 6;
        a = Z.hl;
    }
    x = op >> 6;
    y = (op >> 3) & 7;
    zz = op & 7;
    v = mem ? rb(a) : getr0(zz);

    if (x == 1) { /* BIT */
        u8 xy = mem ? (u8)(a >> 8) : v;
        Z.f = (u8)((Z.f & FLAG_C) | FLAG_H | (xy & (FLAG_X | FLAG_Y)));
        if (!(v & (1 << y)))
            Z.f |= FLAG_Z | FLAG_PV;
        else if (y == 7)
            Z.f |= FLAG_S;
        CYC(mem ? 12 : 8);
        return;
    }
    if (x == 0)
        v = rot(y, v);
    else if (x == 2)
        v &= (u8)~(1 << y);
    else
        v |= (u8)(1 << y);
    if (mem) {
        wb(a, v);
        if (zz != 6)
            setr0(zz, v);
        CYC(15);
    } else {
        setr0(zz, v);
        CYC(8);
    }
}

static void exec_ed(void)
{
    u8 op = fetch(), v;
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    int dir;

    Z.r = (Z.r & 0x80) | ((Z.r + 1) & 0x7F);
    hlp = &Z.hl;

    if (x == 1) {
        switch (zz) {
        case 0:
            v = z80_port_in(Z.bc);
            if (y != 6)
                setr0(y, v);
            Z.f = (Z.f & FLAG_C) | szp[v];
            CYC(12);
            break;
        case 1:
            z80_port_out(Z.bc, y == 6 ? 0 : getr0(y));
            CYC(12);
            break;
        case 2:
            if (q)
                adc16(*rp(p));
            else
                sbc16(*rp(p));
            CYC(15);
            break;
        case 3: {
            u16 a = fetch16();
            if (q)
                *rp(p) = rw(a);
            else
                ww(a, *rp(p));
            CYC(20);
            break;
        }
        case 4:
            v = Z.a;
            Z.a = 0;
            sub8(v, 0);
            CYC(8);
            break;
        case 5:
            Z.pc = pop();
            Z.iff1 = Z.iff2;
            CYC(14);
            break;
        case 6: {
            static const u8 modes[4] = { 0, 0, 1, 2 };
            Z.im = modes[y & 3];
            CYC(8);
            break;
        }
        default:
            switch (y) {
            case 0: Z.i = Z.a; CYC(9); break;
            case 1: Z.r = Z.a; CYC(9); break;
            case 2:
                Z.a = Z.i;
                Z.f = (u8)((Z.f & FLAG_C) | (szp[Z.a] & ~FLAG_PV) | (Z.iff2 ? FLAG_PV : 0));
                CYC(9);
                break;
            case 3:
                Z.a = Z.r;
                Z.f = (u8)((Z.f & FLAG_C) | (szp[Z.a] & ~FLAG_PV) | (Z.iff2 ? FLAG_PV : 0));
                CYC(9);
                break;
            case 4: /* RRD */
                v = rb(Z.hl);
                wb(Z.hl, (Z.a << 4) | (v >> 4));
                Z.a = (Z.a & 0xF0) | (v & 0x0F);
                Z.f = (Z.f & FLAG_C) | szp[Z.a];
                CYC(18);
                break;
            case 5: /* RLD */
                v = rb(Z.hl);
                wb(Z.hl, (v << 4) | (Z.a & 0x0F));
                Z.a = (Z.a & 0xF0) | (v >> 4);
                Z.f = (Z.f & FLAG_C) | szp[Z.a];
                CYC(18);
                break;
            default:
                CYC(8);
                break;
            }
            break;
        }
        return;
    }

    if (x != 2 || zz > 3 || y < 4) {
        CYC(8);
        return;
    }

    /* block instructions */
    dir = (y & 1) ? -1 : 1;
    CYC(16);
    switch (zz) {
    case 0: { /* LDI LDD LDIR LDDR */
        unsigned n;
        v = rb(Z.hl);
        wb(Z.de, v);
        Z.hl += dir;
        Z.de += dir;
        Z.bc--;
        n = v + Z.a;
        Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_C)) | (Z.bc ? FLAG_PV : 0) | (n & FLAG_X) | ((n & 2) ? FLAG_Y : 0));
        if (y >= 6 && Z.bc) {
            Z.pc -= 2;
            CYC(5);
        }
        break;
    }
    case 1: { /* CPI CPD CPIR CPDR */
        unsigned r, n;
        u8 h;
        v = rb(Z.hl);
        r = (Z.a - v) & 0xFF;
        h = (Z.a ^ v ^ r) & FLAG_H;
        Z.hl += dir;
        Z.bc--;
        n = r - (h ? 1 : 0);
        Z.f = (u8)((Z.f & FLAG_C) | FLAG_N | (r & FLAG_S) | (r ? 0 : FLAG_Z) | h | (Z.bc ? FLAG_PV : 0) |
                   (n & FLAG_X) | ((n & 2) ? FLAG_Y : 0));
        if (y >= 6 && Z.bc && r) {
            Z.pc -= 2;
            CYC(5);
        }
        break;
    }
    case 2: { /* INI IND INIR INDR */
        u8 b;
        unsigned k;
        v = z80_port_in(Z.bc);
        wb(Z.hl, v);
        Z.hl += dir;
        b = (u8)((Z.bc >> 8) - 1);
        Z.bc = (u16)((Z.bc & 0xFF) | (b << 8));
        k = v + (((Z.bc & 0xFF) + (unsigned)dir) & 0xFF);
        Z.f = (u8)((szp[b] & ~FLAG_PV) | ((v & 0x80) ? FLAG_N : 0) | (k > 255 ? FLAG_H | FLAG_C : 0) |
                   (szp[(k & 7) ^ b] & FLAG_PV));
        if (y >= 6 && b) {
            Z.pc -= 2;
            CYC(5);
        }
        break;
    }
    default: { /* OUTI OUTD OTIR OTDR */
        u8 b;
        unsigned k;
        v = rb(Z.hl);
        b = (u8)((Z.bc >> 8) - 1);
        Z.bc = (u16)((Z.bc & 0xFF) | (b << 8));
        z80_port_out(Z.bc, v);
        Z.hl += dir;
        k = v + (Z.hl & 0xFF);
        Z.f = (u8)((szp[b] & ~FLAG_PV) | ((v & 0x80) ? FLAG_N : 0) | (k > 255 ? FLAG_H | FLAG_C : 0) |
                   (szp[(k & 7) ^ b] & FLAG_PV));
        if (y >= 6 && b) {
            Z.pc -= 2;
            CYC(5);
        }
        break;
    }
    }
}

/* ---- main decoder ---- */

static void exec(u8 op)
{
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    u16 a, t;
    u8 v;

    switch (x) {
    case 1:
        if (op == 0x76) { /* HALT */
            Z.halted = 1;
            CYC(4);
        } else if (zz == 6) {
            a = hl_addr();
            setr0(y, rb(a));
            CYC(7);
        } else if (y == 6) {
            a = hl_addr();
            wb(a, getr0(zz));
            CYC(7);
        } else {
            setr(y, getr(zz));
            CYC(4);
        }
        return;

    case 2:
        if (zz == 6) {
            v = rb(hl_addr());
            CYC(7);
        } else {
            v = getr(zz);
            CYC(4);
        }
        alu(y, v);
        return;

    case 0:
        switch (zz) {
        case 0:
            if (y == 0) {
                CYC(4);
            } else if (y == 1) {
                v = Z.a; Z.a = Z.a2; Z.a2 = v;
                v = Z.f; Z.f = Z.f2; Z.f2 = v;
                CYC(4);
            } else if (y == 2) { /* DJNZ */
                s8 d = (s8)fetch();
                v = (u8)((Z.bc >> 8) - 1);
                Z.bc = (u16)((Z.bc & 0xFF) | (v << 8));
                if (v) {
                    Z.pc += d;
                    CYC(13);
                } else {
                    CYC(8);
                }
            } else {
                s8 d = (s8)fetch();
                if (y == 3 || cond(y - 4)) {
                    Z.pc += d;
                    CYC(12);
                } else {
                    CYC(7);
                }
            }
            return;
        case 1:
            if (q) {
                add16(hlp, *rp(p));
                CYC(11);
            } else {
                *rp(p) = fetch16();
                CYC(10);
            }
            return;
        case 2:
            switch (y) {
            case 0: wb(Z.bc, Z.a); CYC(7); break;
            case 1: Z.a = rb(Z.bc); CYC(7); break;
            case 2: wb(Z.de, Z.a); CYC(7); break;
            case 3: Z.a = rb(Z.de); CYC(7); break;
            case 4: a = fetch16(); ww(a, *hlp); CYC(16); break;
            case 5: a = fetch16(); *hlp = rw(a); CYC(16); break;
            case 6: a = fetch16(); wb(a, Z.a); CYC(13); break;
            default: a = fetch16(); Z.a = rb(a); CYC(13); break;
            }
            return;
        case 3:
            if (q)
                (*rp(p))--;
            else
                (*rp(p))++;
            CYC(6);
            return;
        case 4:
        case 5:
            if (y == 6) {
                a = hl_addr();
                v = rb(a);
                wb(a, zz == 4 ? inc8(v) : dec8(v));
                CYC(11);
            } else {
                v = getr(y);
                setr(y, zz == 4 ? inc8(v) : dec8(v));
                CYC(4);
            }
            return;
        case 6:
            if (y == 6) {
                a = hl_addr();
                wb(a, fetch());
                CYC(hlp == &Z.hl ? 10 : 7);
            } else {
                setr(y, fetch());
                CYC(7);
            }
            return;
        default:
            switch (y) {
            case 0:
                v = Z.a >> 7;
                Z.a = (u8)((Z.a << 1) | v);
                Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_PV)) | (Z.a & (FLAG_X | FLAG_Y)) | v);
                break;
            case 1:
                v = Z.a & 1;
                Z.a = (u8)((Z.a >> 1) | (v << 7));
                Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_PV)) | (Z.a & (FLAG_X | FLAG_Y)) | v);
                break;
            case 2:
                v = Z.a >> 7;
                Z.a = (u8)((Z.a << 1) | (Z.f & FLAG_C));
                Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_PV)) | (Z.a & (FLAG_X | FLAG_Y)) | v);
                break;
            case 3:
                v = Z.a & 1;
                Z.a = (u8)((Z.a >> 1) | ((Z.f & FLAG_C) << 7));
                Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_PV)) | (Z.a & (FLAG_X | FLAG_Y)) | v);
                break;
            case 4:
                daa();
                break;
            case 5:
                Z.a = (u8)~Z.a;
                Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_PV | FLAG_C)) | FLAG_H | FLAG_N | (Z.a & (FLAG_X | FLAG_Y)));
                break;
            case 6:
                Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_PV)) | FLAG_C | (Z.a & (FLAG_X | FLAG_Y)));
                break;
            default:
                Z.f = (u8)((Z.f & (FLAG_S | FLAG_Z | FLAG_PV)) | ((Z.f & FLAG_C) ? FLAG_H : FLAG_C) |
                           (Z.a & (FLAG_X | FLAG_Y)));
                break;
            }
            CYC(4);
            return;
        }

    default:
        switch (zz) {
        case 0:
            if (cond(y)) {
                Z.pc = pop();
                CYC(11);
            } else {
                CYC(5);
            }
            return;
        case 1:
            if (!q) {
                t = pop();
                if (p == 3) {
                    Z.a = t >> 8;
                    Z.f = (u8)t;
                } else {
                    *rp(p) = t;
                }
                CYC(10);
            } else if (p == 0) {
                Z.pc = pop();
                CYC(10);
            } else if (p == 1) {
                t = Z.bc; Z.bc = Z.bc2; Z.bc2 = t;
                t = Z.de; Z.de = Z.de2; Z.de2 = t;
                t = Z.hl; Z.hl = Z.hl2; Z.hl2 = t;
                CYC(4);
            } else if (p == 2) {
                Z.pc = *hlp;
                CYC(4);
            } else {
                Z.sp = *hlp;
                CYC(6);
            }
            return;
        case 2:
            a = fetch16();
            if (cond(y))
                Z.pc = a;
            CYC(10);
            return;
        case 3:
            switch (y) {
            case 0:
                Z.pc = fetch16();
                CYC(10);
                break;
            case 1:
                exec_cb();
                break;
            case 2:
                v = fetch();
                z80_port_out((u16)((Z.a << 8) | v), Z.a);
                CYC(11);
                break;
            case 3:
                v = fetch();
                Z.a = z80_port_in((u16)((Z.a << 8) | v));
                CYC(11);
                break;
            case 4:
                t = rw(Z.sp);
                ww(Z.sp, *hlp);
                *hlp = t;
                CYC(19);
                break;
            case 5:
                t = Z.de; Z.de = Z.hl; Z.hl = t;
                CYC(4);
                break;
            case 6:
                Z.iff1 = Z.iff2 = 0;
                CYC(4);
                break;
            default:
                Z.iff1 = Z.iff2 = 1;
                Z.ei_delay = 2;
                CYC(4);
                break;
            }
            return;
        case 4:
            a = fetch16();
            if (cond(y)) {
                push(Z.pc);
                Z.pc = a;
                CYC(17);
            } else {
                CYC(10);
            }
            return;
        case 5:
            if (!q) {
                push(p == 3 ? (u16)((Z.a << 8) | Z.f) : *rp(p));
                CYC(11);
            } else if (p == 0) {
                a = fetch16();
                push(Z.pc);
                Z.pc = a;
                CYC(17);
            } else if (p == 2) {
                exec_ed();
            } else { /* a DD or FD following another prefix */
                hlp = p == 1 ? &Z.ix : &Z.iy;
                Z.r = (Z.r & 0x80) | ((Z.r + 1) & 0x7F);
                CYC(4);
                exec(fetch());
            }
            return;
        case 6:
            alu(y, fetch());
            CYC(7);
            return;
        default:
            push(Z.pc);
            Z.pc = (u16)(y * 8);
            CYC(11);
            return;
        }
    }
}

void z80_init(void)
{
    int i, b, par;
    for (i = 0; i < 256; i++) {
        par = 0;
        for (b = 0; b < 8; b++)
            par ^= (i >> b) & 1;
        szp[i] = (u8)((i & (FLAG_S | FLAG_X | FLAG_Y)) | (i ? 0 : FLAG_Z) | (par ? 0 : FLAG_PV));
    }
}

void z80_reset(void)
{
    Z.pc = 0;
    Z.sp = 0xFFFF;
    Z.a = Z.f = 0xFF;
    Z.i = Z.r = 0;
    Z.iff1 = Z.iff2 = 0;
    Z.im = 0;
    Z.halted = 0;
    Z.ei_delay = 0;
}

void z80_run(s32 until)
{
    while (Z.cycles < until) {
        if (Z.ei_delay) {
            Z.ei_delay--;
        }
        if (Z.irq && Z.iff1 && !Z.ei_delay) {
            Z.halted = 0;
            Z.iff1 = Z.iff2 = 0;
            Z.r = (Z.r & 0x80) | ((Z.r + 1) & 0x7F);
            push(Z.pc);
            if (Z.im == 2) {
                Z.pc = rw((u16)((Z.i << 8) | 0xFF));
                CYC(19);
            } else {
                Z.pc = 0x38;
                CYC(13);
            }
            continue;
        }
        if (Z.halted) {
            /* HALT executes NOPs until an interrupt arrives */
            s32 n = (until - Z.cycles + 3) / 4;
            CYC(n * 4);
            Z.r = (u8)((Z.r & 0x80) | ((Z.r + n) & 0x7F));
            break;
        }
        hlp = &Z.hl;
#ifdef Z80_TRACE
        if (z80_trace_hook)
            z80_trace_hook(Z.pc);
#endif
        Z.r = (Z.r & 0x80) | ((Z.r + 1) & 0x7F);
        exec(fetch());
    }
}
