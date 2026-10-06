/* mdcore - a Sega Mega Drive / Genesis emulator core.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
 *
 * Public interface. A front end copies a cartridge image into md_rom_buffer(),
 * calls md_load(), then md_run_frame() once per video frame, and reads the
 * picture and sound back out. Nothing here allocates memory or does I/O.
 */
#ifndef MDCORE_H
#define MDCORE_H

#include <stdint.h>

#define MD_VERSION "0.1.0"
#define MD_ROM_MAX 0x800000         /* 8 MiB */
#define MD_SRAM_MAX 0x10000
#define MD_FB_STRIDE 320            /* pixels per frame buffer row */
#define MD_AUDIO_MAX 1200           /* stereo frames per md_run_frame(), upper bound */

/* Button bits for md_set_input(). */
enum {
    MD_UP = 0x001, MD_DOWN = 0x002, MD_LEFT = 0x004, MD_RIGHT = 0x008,
    MD_B = 0x010, MD_C = 0x020, MD_A = 0x040, MD_START = 0x080,
    MD_Z = 0x100, MD_Y = 0x200, MD_X = 0x400, MD_MODE = 0x800
};

enum { MD_REGION_AUTO = 0, MD_REGION_JP = 1, MD_REGION_US = 2, MD_REGION_EU = 3 };
enum { MD_PIXEL_XRGB8888 = 0, MD_PIXEL_ABGR8888 = 1 };

void md_init(int pixel_format);

/* Cartridge. Returns 1 on success. Accepts raw (.bin/.md/.gen) and
 * interleaved (.smd) images up to MD_ROM_MAX bytes. */
uint8_t *md_rom_buffer(void);
int md_load(uint32_t size, int region);
void md_reset(void);

void md_run_frame(void);
void md_set_input(int port, uint32_t buttons);  /* port 0 or 1 */
void md_set_six_button(int port, int enabled);
void md_set_audio_filter(int enabled);   /* console-style low-pass, default off */

/* Video: 32-bit pixels, MD_FB_STRIDE per row, width x height visible. */
const uint32_t *md_framebuffer(void);
int md_width(void);
int md_height(void);
int md_is_pal(void);
uint32_t md_fps_x1000(void);

/* Audio: interleaved stereo signed 16-bit rendered by the last frame. */
const int16_t *md_audio(void);
int md_audio_frames(void);
uint32_t md_audio_rate_x1000(void);

/* Memory, for save files and for tooling. */
uint8_t *md_ram(void);          /* 64 KiB of 68000 work RAM, big-endian */
uint8_t *md_sram(void);
uint32_t md_sram_size(void);    /* 0 when the cartridge has none */
int md_sram_dirty(void);        /* written since the last call */

/* Save states. md_state_save returns the size written. md_state_load
 * returns 1 on success and leaves the machine untouched on failure. */
uint32_t md_state_size(void);
uint32_t md_state_save(uint8_t *out);
int md_state_load(const uint8_t *in, uint32_t size);
uint8_t *md_state_buffer(void); /* scratch buffer of md_state_size() bytes */

#endif
