/* Native counterpart of wasm_parity.mjs: same session, same hash input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mdcore.h"
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"), *p;
    long n = (long)fread(md_rom_buffer(), 1, MD_ROM_MAX, f);
    int frames = atoi(argv[2]), i;
    fclose(f);
    md_init(0);
    md_load((uint32_t)n, 0);
    p = popen("sha1sum", "w");
    for (i = 0; i < frames; i++) {
        const char *s = argc > 3 ? argv[3] : "";
        uint32_t b = 0;
        while (*s) { int fr = atoi(s); const char *c = strchr(s, ':'); if (fr <= i) b = (uint32_t)strtoul(c + 1, NULL, 16); s = strchr(c, ','); if (!s) break; s++; }
        md_set_input(0, b);
        md_run_frame();
        fwrite(md_audio(), 4, (size_t)md_audio_frames(), p);
    }
    fwrite(md_framebuffer(), 4, 320 * 224, p);
    fwrite(md_ram(), 1, 65536, p);
    printf("native "); fflush(stdout);
    pclose(p);
    return 0;
}
