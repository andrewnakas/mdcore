/* Runs packed SingleStepTests/z80 vectors (see z80_sst_pack.py) against z80.c.
 * WZ and Q are not modelled, so flag bits 3 and 5 are reported separately. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/z80.h"

static u8 mem[0x10000];
static const u8 *ports; static int nports;
u8 z80_mem_read(u16 a) { return mem[a]; }
void z80_mem_write(u16 a, u8 v) { mem[a] = v; }
u8 z80_port_in(u16 p) { int i; for (i = 0; i < nports; i++) if (!ports[i * 4 + 3] && (ports[i * 4] | ports[i * 4 + 1] << 8) == p) return ports[i * 4 + 2]; return 0xFF; }
void z80_port_out(u16 p, u8 v) { (void)p; (void)v; }

static void load(const u16 *r)
{
    memset(&z80, 0, sizeof z80);
    z80.pc = r[0]; z80.sp = r[1]; z80.a = r[2]; z80.bc = r[3] << 8 | r[4]; z80.de = r[5] << 8 | r[6]; z80.f = r[7];
    z80.hl = r[8] << 8 | r[9]; z80.i = r[10]; z80.r = r[11]; z80.ix = r[12]; z80.iy = r[13];
    z80.a2 = r[14] >> 8; z80.f2 = (u8)r[14]; z80.bc2 = r[15]; z80.de2 = r[16]; z80.hl2 = r[17];
    z80.im = r[18]; z80.iff1 = r[19]; z80.iff2 = r[20];
}

int main(int argc, char **argv)
{
    FILE *fp = fopen(argv[1], "rb"); long len; u8 *buf; const u8 *p, *end;
    int verbose = argc > 2 ? atoi(argv[2]) : 0;
    char cur[17] = ""; long pass = 0, fail = 0, xy = 0, cyc = 0, gp = 0, gf = 0, gxy = 0, gc = 0; int shown = 0;
    if (!fp) return 2;
    fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
    buf = malloc(len); if (fread(buf, 1, len, fp) != (size_t)len) return 2;
    z80_init();
    for (p = buf, end = buf + len; ; ) {
        u16 ini[21], fin[21], h[4]; int i, bad = 0; const u8 *ir, *fr; char name[17];
        if (p >= end || memcmp(p, cur, 16)) {
            if (cur[0] && (fail || xy || cyc)) printf("%-10s pass %4ld fail %4ld xy-only %4ld cyc %4ld\n", cur, pass, fail, xy, cyc);
            gp += pass; gf += fail; gxy += xy; gc += cyc; pass = fail = xy = cyc = 0; shown = 0;
            if (p >= end) break;
            memcpy(cur, p, 16);
        }
        memcpy(name, p, 16); name[16] = 0; p += 16;
        memcpy(ini, p, 42); p += 42; memcpy(fin, p, 42); p += 42; memcpy(h, p, 8); p += 8;
        ir = p; p += h[1] * 4; fr = p; p += h[2] * 4; ports = p; nports = h[3]; p += h[3] * 4;
        for (i = 0; i < h[1]; i++) mem[ir[i * 4] | ir[i * 4 + 1] << 8] = ir[i * 4 + 2];
        load(ini);
        z80_run(1);
        {
            u16 got[21] = { z80.pc, z80.sp, z80.a, z80.bc >> 8, z80.bc & 255, z80.de >> 8, z80.de & 255, z80.f, z80.hl >> 8, z80.hl & 255,
                z80.i, z80.r, z80.ix, z80.iy, (u16)(z80.a2 << 8 | z80.f2), z80.bc2, z80.de2, z80.hl2, z80.im, z80.iff1, z80.iff2 };
            int fonly = 1;
            for (i = 0; i < 21; i++) if (got[i] != fin[i]) { if (i == 7 && ((got[i] ^ fin[i]) & 0xD7) == 0) bad |= 1; else { bad |= 2; fonly = 0; } }
            for (i = 0; i < h[2]; i++) if (mem[fr[i * 4] | fr[i * 4 + 1] << 8] != fr[i * 4 + 2]) bad |= 4;
            (void)fonly;
            if (bad & 6) {
                fail++;
                if (shown++ < verbose) {
                    static const char *nm[21] = { "pc","sp","a","b","c","d","e","f","h","l","i","r","ix","iy","af'","bc'","de'","hl'","im","iff1","iff2" };
                    printf("  FAIL %s:", name);
                    for (i = 0; i < 21; i++) if (got[i] != fin[i]) printf(" %s %x->%x want %x", nm[i], ini[i], got[i], fin[i]);
                    if (bad & 4) printf(" [mem]");
                    printf("\n");
                }
            } else {
                pass++;
                if (bad & 1) xy++;
                if (z80.cycles != h[0]) { cyc++; if (shown++ < verbose) printf("  CYC %s got %d want %d\n", name, z80.cycles, h[0]); }
            }
        }
        for (i = 0; i < h[1]; i++) mem[ir[i * 4] | ir[i * 4 + 1] << 8] = 0;
        for (i = 0; i < h[2]; i++) mem[fr[i * 4] | fr[i * 4 + 1] << 8] = 0;
    }
    printf("TOTAL pass %ld fail %ld (of the passes: flag bits 3/5 differ %ld, cycle mismatch %ld)\n", gp, gf, gxy, gc);
    return gf != 0;
}
