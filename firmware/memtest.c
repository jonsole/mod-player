/* A quick SDRAM test, sized for simulation as well as the board: every
 * address line, a spread of words across all banks and rows, a dense
 * block, and byte and halfword writes. */
#include "memtest.h"

#define W(a) (*(volatile uint32_t *)(a))
#define H(a) (*(volatile uint16_t *)(a))
#define B(a) (*(volatile uint8_t *)(a))

static uint32_t pattern(uintptr_t a)
{
    return (uint32_t)a * 2654435761u ^ 0xA5C3F00Fu;
}

uintptr_t memtest(uintptr_t base, uint32_t size)
{
    /* address lines: each power of two must reach its own word */
    for (uint32_t off = 4; off < size; off <<= 1)
        W(base + off) = pattern(off);
    W(base) = pattern(0);
    for (uint32_t off = 4; off < size; off <<= 1)
        if (W(base + off) != pattern(off))
            return base + off;
    if (W(base) != pattern(0))
        return base;

    /* spread: one word every 4 KB + 4, covering all banks and rows */
    for (uint32_t off = 0; off < size; off += 4100)
        W(base + off) = ~pattern(off);
    for (uint32_t off = 0; off < size; off += 4100)
        if (W(base + off) != ~pattern(off))
            return base + off;

    /* dense block */
    for (uint32_t off = 0; off < 4096; off += 4)
        W(base + off) = pattern(off) + 1;
    for (uint32_t off = 0; off < 4096; off += 4)
        if (W(base + off) != pattern(off) + 1)
            return base + off;

    /* byte and halfword writes leave their neighbours alone */
    uintptr_t a = base + 0x123450;
    W(a) = 0x11223344u;
    B(a + 1) = 0xAB;
    B(a + 3) = 0xCD;
    if (W(a) != 0xCD22AB44u) return a;
    H(a + 2) = 0x5566;
    if (W(a) != 0x5566AB44u || H(a) != 0xAB44 || B(a + 2) != 0x66) return a;
    return 0;
}
