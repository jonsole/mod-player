/* SPI flash commands (see flash.h). */
#include "flash.h"

#include "hw.h"

static void wait_idle(void)
{
    while (SPI_CTRL & SPI_BUSY)
        ;
}

static void select(int on)
{
    wait_idle();
    SPI_CTRL = on ? 1u : 0u;
}

static uint8_t transfer(uint8_t out)
{
    wait_idle();
    SPI_DATA = out;
    wait_idle();
    return (uint8_t)SPI_DATA;
}

void flash_read_id(uint8_t id[3])
{
    select(1);
    transfer(0x9F);
    for (int i = 0; i < 3; i++)
        id[i] = transfer(0);
    select(0);
}
