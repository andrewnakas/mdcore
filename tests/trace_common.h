/* Shared by trace.c (mdcore) and oracle/oracle.c (reference): input scripts,
 * hashing, and dumps, so both sides produce directly comparable output.
 *
 * Script: one "frame mask" pair per line (mask in hex, mdcore MD_* bits),
 * held from that frame until the next line. '#' starts a comment.
 * Pixels are compared as 4-bit-per-channel VDP intensity levels
 * (shadow c, normal 2c, highlight c+7), which both emulators can produce
 * exactly regardless of their RGB palettes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { int frame; uint32_t mask; } ev_t;
static ev_t evs[100000];
static int nev;

static void script_load(const char *path)
{
    char line[256];
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int fr; unsigned m;
        if (line[0] == '#') continue;
        if (sscanf(line, "%d %x", &fr, &m) == 2 && nev < 100000) { evs[nev].frame = fr; evs[nev].mask = m; nev++; }
    }
    fclose(f);
}

static uint32_t script_mask(int frame)
{
    uint32_t m = 0; int i;
    for (i = 0; i < nev && evs[i].frame <= frame; i++) m = evs[i].mask;
    return m;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

/* TRACE_MASK="FD00-FE00,..." excludes work RAM ranges from the hash, e.g.
 * stack residue, so a later and more meaningful divergence can be found. */
static void mask_ram(uint8_t *dst, const uint8_t *ram)
{
    const char *m = getenv("TRACE_MASK");
    memcpy(dst, ram, 0x10000);
    while (m && *m) {
        unsigned a, b; int n = 0;
        if (sscanf(m, "%x-%x%n", &a, &b, &n) != 2) break;
        if (a < b && b <= 0x10000) memset(dst + a, 0, b - a);
        m += n; if (*m == ',') m++;
    }
}

/* levels: w*h*3 bytes, 0..15 each. ram: 64 KiB big-endian. */
static void trace_emit(int frame, int w, int h, const uint8_t *levels, const uint8_t *ram_in)
{
    static uint8_t ram[0x10000];
    mask_ram(ram, ram_in);
    uint64_t v = fnv(14695981039346656037ull, &w, sizeof w);
    v = fnv(v, &h, sizeof h);
    v = fnv(v, levels, (size_t)w * h * 3);
    printf("%d %dx%d %016llx %016llx\n", frame, w, h, (unsigned long long)v,
           (unsigned long long)fnv(14695981039346656037ull, ram, 0x10000));
}

static void dump_frame(const char *prefix, int w, int h, const uint8_t *levels, const uint8_t *ram)
{
    char path[512]; FILE *f; int i;
    snprintf(path, sizeof path, "%s.ppm", prefix);
    f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (i = 0; i < w * h * 3; i++) fputc(levels[i] * 17, f);
    fclose(f);
    snprintf(path, sizeof path, "%s.ram", prefix);
    f = fopen(path, "wb"); fwrite(ram, 1, 0x10000, f); fclose(f);
}
