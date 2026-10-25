#ifndef MEMTEST_H
#define MEMTEST_H
#include <stdint.h>
/* Where a memory test failed: the word address, what was expected there and
 * what was read back. */
struct memtest_fail {
    uintptr_t addr;
    uint32_t expected, got;
};
/* Tests size bytes of RAM at base. Returns 0 if good; otherwise 1, with the
 * first failure in *fail. */
int memtest(uintptr_t base, uint32_t size, struct memtest_fail *fail);
#endif
