/* Runs the SingleStepTests/m68000 vectors (.json.bin) against m68k.c.
 * Test-only: the vectors are not part of mdcore and are not redistributed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/m68k.h"

static u8 *mem;
u32 m68k_bus_read8(u32 a) { return mem[a & 0xFFFFFF]; }
u32 m68k_bus_read16(u32 a) { a &= 0xFFFFFE; return (mem[a] << 8) | mem[a + 1]; }
void m68k_bus_write8(u32 a, u32 v) { mem[a & 0xFFFFFF] = (u8)v; }
void m68k_bus_write16(u32 a, u32 v) { a &= 0xFFFFFE; mem[a] = v >> 8; mem[a + 1] = (u8)v; }
void m68k_irq_ack(int l) { (void)l; }

typedef struct { u32 r[19]; u32 pf[2]; u32 nram; const u8 *ram; } state_t;
static const u8 *p;
static u32 u32le(void) { u32 v; memcpy(&v, p, 4); p += 4; return v; }

static void rd_state(state_t *s)
{
    int i;
    p += 8;
    for (i = 0; i < 19; i++) s->r[i] = u32le();
    s->pf[0] = u32le(); s->pf[1] = u32le();
    s->nram = u32le(); s->ram = p; p += 6 * s->nram;
}

int main(int argc, char **argv)
{
    int f, verbose = getenv("V") ? atoi(getenv("V")) : 0;
    long gpass = 0, gfail = 0, gskip = 0, gcyc = 0;
    mem = calloc(1, 0x1000002);
    for (f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        long len; u8 *buf; u32 n, t; long pass = 0, fail = 0, skip = 0, cyc = 0; int shown = 0;
        if (!fp) { perror(argv[f]); continue; }
        fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
        buf = malloc(len); if (fread(buf, 1, len, fp) != (size_t)len) return 1; fclose(fp);
        p = buf; p += 4; n = u32le();
        for (t = 0; t < n; t++) {
            state_t ini, fin; u32 nl, ncyc, ntr, i; int aerr = 0, bad = 0; char name[128];
            p += 8; p += 8; nl = u32le(); memcpy(name, p, nl < 127 ? nl : 127); name[nl < 127 ? nl : 127] = 0; p += nl;
            rd_state(&ini); rd_state(&fin);
            p += 8; ncyc = u32le(); ntr = u32le();
            for (i = 0; i < ntr; i++) { u8 tw = *p; p += 5; if (tw) p += 20; if (tw >= 4) aerr = 1; }
            if (aerr) { skip++; continue; }
            for (i = 0; i < ini.nram; i++) { u32 a; u16 d; memcpy(&a, ini.ram + i * 6, 4); memcpy(&d, ini.ram + i * 6 + 4, 2); mem[a] = d >> 8; mem[a | 1] = (u8)d; }
            memset(&m68k, 0, sizeof m68k);
            m68k.tas_writes = 1;
            memcpy(m68k.d, ini.r, 32); memcpy(m68k.a, ini.r + 8, 28);
            m68k.s = (ini.r[17] >> 13) & 1;
            m68k.a[7] = m68k.s ? ini.r[16] : ini.r[15];
            m68k.sp_other = m68k.s ? ini.r[15] : ini.r[16];
            { u32 sr = ini.r[17]; m68k.x = sr >> 4 & 1; m68k.n = sr >> 3 & 1; m68k.z = sr >> 2 & 1; m68k.v = sr >> 1 & 1; m68k.c = sr & 1; m68k.t = sr >> 15 & 1; m68k.imask = sr >> 8 & 7; }
            m68k.pc = ini.r[18] - 4;
            m68k_run(1);
            {
                u32 usp = m68k.s ? m68k.sp_other : m68k.a[7], ssp = m68k.s ? m68k.a[7] : m68k.sp_other;
                u32 sr = m68k_get_sr();
                for (i = 0; i < 8; i++) if (m68k.d[i] != fin.r[i]) bad |= 1;
                for (i = 0; i < 7; i++) if (m68k.a[i] != fin.r[8 + i]) bad |= 2;
                if (usp != fin.r[15] || ssp != fin.r[16]) bad |= 4;
                if (sr != fin.r[17]) bad |= 8;
                if (!m68k.stopped && m68k.pc != fin.r[18] - 4) bad |= 16;
                for (i = 0; i < fin.nram; i++) { u32 a; u16 d; memcpy(&a, fin.ram + i * 6, 4); memcpy(&d, fin.ram + i * 6 + 4, 2); if (mem[a] != (d >> 8) || mem[a | 1] != (u8)d) bad |= 32; }
                if (bad) {
                    fail++;
                    if (shown++ < verbose) {
                        printf("  FAIL %s [%s] bad=%x op=%04x sr %04x->%04x want %04x pc %x want %x cyc %d want %u\n", argv[f], name, bad, ini.pf[0], ini.r[17], sr, fin.r[17], m68k.pc, fin.r[18] - 4, m68k.cycles, ncyc);
                        for (i = 0; i < 8; i++) if (m68k.d[i] != fin.r[i]) printf("    d%u %08x->%08x want %08x\n", i, ini.r[i], m68k.d[i], fin.r[i]);
                        for (i = 0; i < 7; i++) if (m68k.a[i] != fin.r[8 + i]) printf("    a%u %08x->%08x want %08x\n", i, ini.r[8 + i], m68k.a[i], fin.r[8 + i]);
                        if (bad & 4) printf("    usp %08x want %08x ssp %08x want %08x\n", usp, fin.r[15], ssp, fin.r[16]);
                        for (i = 0; i < fin.nram; i++) { u32 a; u16 d; memcpy(&a, fin.ram + i * 6, 4); memcpy(&d, fin.ram + i * 6 + 4, 2); if (mem[a] != (d >> 8) || mem[a | 1] != (u8)d) printf("    mem %06x = %02x%02x want %04x\n", a, mem[a], mem[a | 1], d); }
                    }
                } else {
                    pass++;
                    if ((u32)m68k.cycles != ncyc) {
                        cyc++;
                        if (shown++ < verbose) printf("  CYC  %s [%s] op=%04x got %d want %u\n", argv[f], name, ini.pf[0], m68k.cycles, ncyc);
                    }
                }
            }
            for (i = 0; i < ini.nram; i++) { u32 a; memcpy(&a, ini.ram + i * 6, 4); mem[a] = mem[a | 1] = 0; }
            for (i = 0; i < fin.nram; i++) { u32 a; memcpy(&a, fin.ram + i * 6, 4); mem[a] = mem[a | 1] = 0; }
        }
        { const char *b = strrchr(argv[f], '/'); printf("%-22s pass %5ld fail %5ld cyc-mismatch %5ld skipped(addr err) %ld\n", b ? b + 1 : argv[f], pass, fail, cyc, skip); }
        gpass += pass; gfail += fail; gskip += skip; gcyc += cyc;
        free(buf);
    }
    printf("TOTAL pass %ld fail %ld cycle-mismatch %ld skipped %ld\n", gpass, gfail, gcyc, gskip);
    return gfail != 0;
}
