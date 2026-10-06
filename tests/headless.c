/* Native test driver: runs a ROM for N frames with scripted input, then
 * writes the last frame as a PPM and prints a few bytes of work RAM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mdcore.h"

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    int frames = argc > 2 ? atoi(argv[2]) : 600, i, x, y;
    const char *script = argc > 4 ? argv[4] : "";
    if (argc < 2) { fprintf(stderr, "usage: headless rom [frames] [out.ppm] [script] [peek...]\n"); return 2; }
    md_init(MD_PIXEL_XRGB8888);
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    n = (long)fread(md_rom_buffer(), 1, MD_ROM_MAX + 1, f);
    fclose(f);
    if (!md_load((uint32_t)n, 0)) { fprintf(stderr, "load failed\n"); return 1; }
    for (i = 0; i < frames; i++) {
        /* script: "frame:buttons,frame:buttons" - buttons held from that frame on */
        const char *s = script;
        uint32_t b = 0;
        while (*s) {
            int fr = atoi(s);
            const char *c = strchr(s, ':');
            if (!c) break;
            if (fr <= i) b = (uint32_t)strtoul(c + 1, NULL, 16);
            s = strchr(c, ',');
            if (!s) break;
            s++;
        }
        md_set_input(0, b);
        md_run_frame();
    }
    if (argc > 3 && argv[3][0]) {
        const uint32_t *fb = md_framebuffer();
        f = fopen(argv[3], "wb");
        fprintf(f, "P6 %d %d 255\n", md_width(), md_height());
        for (y = 0; y < md_height(); y++)
            for (x = 0; x < md_width(); x++) {
                uint32_t p = fb[y * MD_FB_STRIDE + x];
                fputc((p >> 16) & 255, f); fputc((p >> 8) & 255, f); fputc(p & 255, f);
            }
        fclose(f);
    }
    printf("frames %d size %dx%d audio %d", frames, md_width(), md_height(), md_audio_frames());
    for (i = 5; i < argc; i++) {
        unsigned a = (unsigned)strtoul(argv[i], NULL, 16) & 0xFFFF;
        printf(" [%04X]=%02X", a, md_ram()[a]);
    }
    printf("\n");
    return 0;
}
