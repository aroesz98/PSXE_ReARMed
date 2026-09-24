/*
    memset for the whole program, in place of newlib-nano's.

    The one newlib-nano brings stores one byte per loop pass and runs from XIP
    flash (utilities/fsl_memcpy.S replaces its memcpy for the same reason). The
    emulator clears buffers all the time - the sound alone a few hundred bytes
    for every 32 samples, which with the byte loop came to ~50 core cycles a
    sample. This one stores words (four at a time) between the unaligned ends,
    so it is safe on device memory too, and it sits in ITCM.

    The optimize attribute keeps GCC from turning its loops back into a call to
    memset.
*/
#include <stddef.h>
#include <stdint.h>

__attribute__((section(".ramfunc.$SRAM_ITC"), optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t b = (uint8_t)c;

    while (n && ((uintptr_t)d & 3u))
    {
        *d++ = b;
        n--;
    }

    if (n >= 4u)
    {
        const uint32_t w = (uint32_t)b * 0x01010101u;
        uint32_t *p = (uint32_t *)d;

        while (n >= 16u)
        {
            p[0] = w;
            p[1] = w;
            p[2] = w;
            p[3] = w;
            p += 4;
            n -= 16u;
        }

        while (n >= 4u)
        {
            *p++ = w;
            n -= 4u;
        }

        d = (uint8_t *)p;
    }

    while (n)
    {
        *d++ = b;
        n--;
    }

    return dst;
}
