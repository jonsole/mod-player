#ifndef MEMTEST_H
#define MEMTEST_H
#include <stdint.h>
/* Tests size bytes of RAM at base. Returns 0 if good, otherwise the first
 * failing address (never 0 for SDRAM, which starts at 0x10000000). */
uintptr_t memtest(uintptr_t base, uint32_t size);
#endif
