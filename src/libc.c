/* mdcore - the two C library routines the core uses, for freestanding builds.
 * Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE. */
#include "common.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}
