/* SPI flash commands (see flash.h). */
#include "flash.h"

#include "crc32.h"
#include "hw.h"

#define CMD_READ_ID   0x9Fu
#define CMD_STATUS    0x05u
#define CMD_WRITE_EN  0x06u
#define CMD_READ      0x03u
#define CMD_PROGRAM   0x02u
#define CMD_ERASE_4K  0x20u
#define CMD_ERASE_64K 0xD8u
#define STATUS_BUSY   0x01u

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

static void command_addr(uint8_t cmd, uint32_t addr)
{
    transfer(cmd);
    transfer((uint8_t)(addr >> 16));
    transfer((uint8_t)(addr >> 8));
    transfer((uint8_t)addr);
}

static void simple(uint8_t cmd)
{
    select(1);
    transfer(cmd);
    select(0);
}

/* Program and erase run inside the chip; poll its status until done. */
static void wait_ready(void)
{
    uint8_t status;
    do {
        select(1);
        transfer(CMD_STATUS);
        status = transfer(0);
        select(0);
    } while (status & STATUS_BUSY);
}

void flash_read_id(uint8_t id[3])
{
    select(1);
    transfer(CMD_READ_ID);
    for (int i = 0; i < 3; i++)
        id[i] = transfer(0);
    select(0);
}

void flash_erase(uint32_t addr, uint32_t len)
{
    uint32_t end = addr + len;
    while (addr < end) {
        int block = (addr % FLASH_BLOCK) == 0 && end - addr >= FLASH_BLOCK;
        simple(CMD_WRITE_EN);
        select(1);
        command_addr(block ? CMD_ERASE_64K : CMD_ERASE_4K, addr);
        select(0);
        wait_ready();
        addr += block ? FLASH_BLOCK : FLASH_SECTOR;
    }
}

void flash_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    while (len > 0) {
        uint32_t n = FLASH_PAGE - (addr % FLASH_PAGE);    /* to the end of this page */
        if (n > len)
            n = len;
        simple(CMD_WRITE_EN);
        select(1);
        command_addr(CMD_PROGRAM, addr);
        for (uint32_t i = 0; i < n; i++)
            transfer(data[i]);
        select(0);
        wait_ready();
        addr += n;
        data += n;
        len -= n;
    }
}

/* Reads len bytes from addr, storing them at dst if store is set (dst may
 * be address 0: the boot loader copies firmware there). Returns their CRC. */
static uint32_t read_range(uint32_t addr, uint8_t *dst, uint32_t len, int store)
{
    uint32_t crc = 0xFFFFFFFFu;
    select(1);
    command_addr(CMD_READ, addr);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = transfer(0);
        if (store)
            dst[i] = b;
        crc = crc32_update(crc, b);
    }
    select(0);
    return ~crc;
}

uint32_t flash_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    return read_range(addr, dst, len, 1);
}

uint32_t flash_crc(uint32_t addr, uint32_t len)
{
    return read_range(addr, 0, len, 0);
}
