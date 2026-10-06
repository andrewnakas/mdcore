/* mdcore side of the oracle comparison. See trace_common.h.
 *   trace rom frames script [audio.raw] [dump_frame dump_prefix] */
#include "trace_common.h"
#include "../src/mdcore.h"
#include "../src/vdp.h"
#include "../src/sound.h"
#ifdef M68K_TRACE
#include "../src/m68k.h"
static FILE *pclog;
static void log_pc(u32 pc) { uint32_t v = pc & 0xFFFFFF; fwrite(&v, 4, 1, pclog); }
#include "../src/z80.h"
static void log_zpc(u16 pc) { uint32_t v = 0x40000000u | pc; fwrite(&v, 4, 1, pclog); }
#endif

static uint8_t lv[320 * 480 * 3];
static uint8_t rev[256];

int main(int argc, char **argv)
{
    FILE *f, *au = NULL;
    long n; int frames, i, x, y, dump = -1;
    static const uint8_t normal[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
    static const uint8_t shadow[8] = { 0, 29, 52, 70, 87, 101, 116, 130 };
    static const uint8_t hilite[8] = { 130, 144, 158, 172, 187, 206, 228, 255 };
    if (argc < 4) { fprintf(stderr, "usage: trace rom frames script [audio.raw] [dump_frame prefix]\n"); return 2; }
    for (i = 0; i < 8; i++) { rev[normal[i]] = (uint8_t)(2 * i); rev[shadow[i]] = (uint8_t)i; rev[hilite[i]] = (uint8_t)(i + 7); }
    f = fopen(argv[1], "rb"); if (!f) return 2;
    md_init(MD_PIXEL_XRGB8888);
    n = (long)fread(md_rom_buffer(), 1, MD_ROM_MAX, f); fclose(f);
    md_set_six_button(0, 0); md_set_six_button(1, 0);
    if (!md_load((uint32_t)n, 0)) return 1;
#ifdef M68K_TRACE
    if (getenv("TRACE_PCLOG")) { pclog = fopen(getenv("TRACE_PCLOG"), "wb"); m68k_trace_hook = log_pc; z80_trace_hook = log_zpc; }
#endif
    if (getenv("TRACE_SOUNDMASK")) sound_mask = (u32)strtoul(getenv("TRACE_SOUNDMASK"), NULL, 16);
    frames = atoi(argv[2]);
    script_load(argv[3]);
    if (argc > 4 && *argv[4]) au = fopen(argv[4], "wb");
    if (argc > 6) dump = atoi(argv[5]);
    for (i = 0; i < frames; i++) {
        const uint32_t *fb; int w, h;
        md_set_input(0, script_mask(i));
#ifdef M68K_TRACE
        if (pclog) { uint32_t m[2] = { 0xFFFFFFFFu, (uint32_t)i }; fwrite(m, 4, 2, pclog); }
#endif
        md_run_frame();
        if (au) fwrite(md_audio(), 4, (size_t)md_audio_frames(), au);
        fb = md_framebuffer(); w = md_width(); h = md_height();
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint32_t p = fb[y * MD_FB_STRIDE + x]; uint8_t *o = lv + (y * w + x) * 3;
                o[0] = rev[(p >> 16) & 255]; o[1] = rev[(p >> 8) & 255]; o[2] = rev[p & 255];
            }
        trace_emit(i, w, h, lv, md_ram());
        if (i == dump) {
            char p[512]; FILE *o;
            dump_frame(argv[6], w, h, lv, md_ram());
            snprintf(p, sizeof p, "%s.vram", argv[6]); o = fopen(p, "wb"); fwrite(vdp.vram, 1, 0x10000, o); fclose(o);
        }
    }
    if (au) fclose(au);
    return 0;
}
