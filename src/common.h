/* mdcore - a Sega Mega Drive / Genesis emulator core.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE. */
#ifndef MDCORE_COMMON_H
#define MDCORE_COMMON_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* The core is freestanding: these two are supplied by libc.c on targets
 * without a C library (the bare wasm32 build). */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#endif
