/* Reference side of the oracle comparison: drives the genesis_plus_gx
 * libretro core (built locally, test-only, never shipped) with the same
 * script and the same output format as ../trace.c.
 *   oracle core.so rom frames script [audio.raw] [dump_frame prefix] */
#include <dlfcn.h>
#include "../trace_common.h"
#include "libretro.h"

static int pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint8_t lv[320 * 480 * 3];
static int vw, vh;
static uint32_t cur;
static FILE *au;
static char sysdir[512];
static FILE *pclog;
static void hook(int type, int width, unsigned addr, unsigned val)
{
    (void)width; (void)val;
    if (type == 1) { uint32_t v = addr & 0xFFFFFF; fwrite(&v, 4, 1, pclog); } /* HOOK_M68K_E */
    else if (type == 512) { uint32_t v = 0x40000000u | (addr & 0xFFFF); fwrite(&v, 4, 1, pclog); } /* HOOK_Z80_E */
}

static const struct { const char *k, *v; } opts[] = {
    { "genesis_plus_gx_system_hw", "mega drive / genesis" },
    { "genesis_plus_gx_region_detect", "auto" },
    { "genesis_plus_gx_bios", "disabled" },
    { "genesis_plus_gx_ym2612", "nuked (ym2612)" },
    { "genesis_plus_gx_audio_filter", "disabled" },
    { "genesis_plus_gx_sound_output", "stereo" },
    { "genesis_plus_gx_blargg_ntsc_filter", "disabled" },
    { "genesis_plus_gx_overscan", "disabled" },
    { "genesis_plus_gx_left_border", "disabled" },
    { "genesis_plus_gx_render", "single field" },
    { "genesis_plus_gx_no_sprite_limit", "disabled" },
    { "genesis_plus_gx_overclock", "100%" },
    { "genesis_plus_gx_force_dtack", "enabled" },
    { "genesis_plus_gx_addr_error", "enabled" },
    { "genesis_plus_gx_lock_on", "disabled" },
    { "genesis_plus_gx_enhanced_vscroll", "disabled" },
    { "genesis_plus_gx_frameskip", "disabled" },
};

static bool env(unsigned cmd, void *data)
{
    unsigned i;
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: pixfmt = *(int *)data; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        {   /* ORACLE_OPT="key=value;key=value" overrides */
            static char buf[4096]; const char *o = getenv("ORACLE_OPT"); char *p, *q;
            if (o) {
                snprintf(buf, sizeof buf, "%s", o);
                for (p = strtok(buf, ";"); p; p = strtok(NULL, ";"))
                    if ((q = strchr(p, '=')) && (size_t)(q - p) == strlen(v->key) && !strncmp(p, v->key, q - p)) {
                        static char val[256]; snprintf(val, sizeof val, "%s", q + 1); v->value = val; return true;
                    }
            }
        }
        for (i = 0; i < sizeof opts / sizeof opts[0]; i++)
            if (!strcmp(v->key, opts[i].k)) { v->value = opts[i].v; return true; }
        v->value = NULL; return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = sysdir; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    default: return false;
    }
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    unsigned x, y;
    if (!data) return;
    vw = (int)w; vh = (int)h;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint8_t *o = lv + (y * w + x) * 3;
            if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p = ((const uint32_t *)((const uint8_t *)data + y * pitch))[x];
                o[0] = (p >> 20) & 15; o[1] = (p >> 12) & 15; o[2] = (p >> 4) & 15;
            } else if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
                o[0] = (p >> 12) & 15; o[1] = (p >> 7) & 15; o[2] = (p >> 1) & 15;
            } else {
                uint16_t p = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
                o[0] = (p >> 11) & 15; o[1] = (p >> 6) & 15; o[2] = (p >> 1) & 15;
            }
        }
}

static size_t audio_batch(const int16_t *d, size_t n) { if (au) fwrite(d, 4, n, au); return n; }
static void audio_one(int16_t l, int16_t r) { int16_t s[2] = { l, r }; audio_batch(s, 1); }
static void poll(void) {}

/* mdcore bits -> libretro joypad bits (gpgx: Y=A, B=B, A=C) */
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    static const struct { uint32_t md; unsigned rid; } map[] = {
        { 0x001, RETRO_DEVICE_ID_JOYPAD_UP }, { 0x002, RETRO_DEVICE_ID_JOYPAD_DOWN },
        { 0x004, RETRO_DEVICE_ID_JOYPAD_LEFT }, { 0x008, RETRO_DEVICE_ID_JOYPAD_RIGHT },
        { 0x040, RETRO_DEVICE_ID_JOYPAD_Y }, { 0x010, RETRO_DEVICE_ID_JOYPAD_B },
        { 0x020, RETRO_DEVICE_ID_JOYPAD_A }, { 0x080, RETRO_DEVICE_ID_JOYPAD_START },
    };
    int16_t m = 0; unsigned i;
    (void)dev; (void)idx;
    if (port != 0) return 0;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (cur & map[i].md) m |= (int16_t)(1 << map[i].rid);
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return m;
    return (m >> id) & 1;
}

#define SYM(name) name = (__typeof__(name))dlsym(h, #name); if (!name) { fprintf(stderr, "missing %s\n", #name); return 2; }

int main(int argc, char **argv)
{
    void *h;
    void (*retro_set_environment)(retro_environment_t), (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t), (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t), (*retro_set_input_state)(retro_input_state_t);
    void (*retro_init)(void), (*retro_run)(void), (*retro_set_controller_port_device)(unsigned, unsigned);
    bool (*retro_load_game)(const struct retro_game_info *);
    void *(*retro_get_memory_data)(unsigned);
    size_t (*retro_get_memory_size)(unsigned);
    size_t (*retro_serialize_size)(void);
    bool (*retro_serialize)(void *, size_t);
    struct retro_game_info gi;
    static uint8_t rom[8 << 20], ram[0x10000];
    FILE *f; int frames, i, dump = -1;
    const uint8_t *wram;

    if (argc < 5) { fprintf(stderr, "usage: oracle core.so rom frames script [audio.raw] [dump_frame prefix]\n"); return 2; }
    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample)
    SYM(retro_set_audio_sample_batch) SYM(retro_set_input_poll) SYM(retro_set_input_state)
    SYM(retro_init) SYM(retro_run) SYM(retro_load_game) SYM(retro_get_memory_data)
    SYM(retro_set_controller_port_device) SYM(retro_get_memory_size) SYM(retro_serialize_size) SYM(retro_serialize)
    snprintf(sysdir, sizeof sysdir, "%s", "/tmp");
    retro_set_environment(env);
    retro_set_video_refresh(video);
    retro_set_audio_sample(audio_one);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(poll);
    retro_set_input_state(input);
    retro_init();
    f = fopen(argv[2], "rb"); if (!f) return 2;
    memset(&gi, 0, sizeof gi);
    gi.path = argv[2]; gi.data = rom; gi.size = fread(rom, 1, sizeof rom, f); fclose(f);
    if (!retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
    retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0));
    retro_set_controller_port_device(1, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0));
    wram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    if (getenv("TRACE_PCLOG")) {
        void (*set_hook)(void (*)(int, int, unsigned, unsigned)) = (void (*)(void (*)(int, int, unsigned, unsigned)))dlsym(h, "set_cpu_hook");
        if (!set_hook) { fprintf(stderr, "core has no set_cpu_hook (use gpgx-hook)\n"); return 2; }
        pclog = fopen(getenv("TRACE_PCLOG"), "wb");
        set_hook(hook);
    }
    frames = atoi(argv[3]);
    script_load(argv[4]);
    if (argc > 5 && *argv[5]) au = fopen(argv[5], "wb");
    if (argc > 7) dump = atoi(argv[6]);
    for (i = 0; i < frames; i++) {
        int k;
        cur = script_mask(i);
        if (pclog) { uint32_t m[2] = { 0xFFFFFFFFu, (uint32_t)i }; fwrite(m, 4, 2, pclog); }
        retro_run();
        for (k = 0; k < 0x10000; k++) ram[k] = wram[k ^ 1]; /* gpgx stores RAM byte-swapped */
        trace_emit(i, vw, vh, lv, ram);
        if (i == dump) {
            dump_frame(argv[7], vw, vh, lv, ram);
            {
                size_t n = retro_serialize_size(); void *buf = malloc(n); char p[512]; FILE *o;
                retro_serialize(buf, n);
                snprintf(p, sizeof p, "%s.state", argv[7]); o = fopen(p, "wb"); fwrite(buf, 1, n, o); fclose(o); free(buf);
            }
            if (retro_get_memory_size(RETRO_MEMORY_VIDEO_RAM) >= 0x10000) {
                char p[512]; const uint8_t *vr = retro_get_memory_data(RETRO_MEMORY_VIDEO_RAM); FILE *o; int k;
                snprintf(p, sizeof p, "%s.vram", argv[7]); o = fopen(p, "wb");
                for (k = 0; k < 0x10000; k++) fputc(vr[k ^ 1], o); /* stored byte-swapped, like work RAM */
                fclose(o);
            }
        }
    }
    if (au) fclose(au);
    return 0;
}
