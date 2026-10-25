/* The board's SPI flash, through the SoC's SPI master (hw/rtl/spi_master.v).
 * A Macronix MX25L6406: 8 MB, 4 KB sectors, 64 KB blocks, 256-byte pages. */
#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

#define FLASH_SIZE   (8u << 20)
#define FLASH_SECTOR 4096u
#define FLASH_BLOCK  65536u
#define FLASH_PAGE   256u

/* Reads the JEDEC ID: manufacturer, memory type, capacity (the board's
 * Macronix MX25L6406 is c2 20 17). All 00 or all ff means no flash is
 * answering. */
void flash_read_id(uint8_t id[3]);

/* Erases every 4 KB sector that [addr, addr + len) touches, using 64 KB
 * block erases where a whole block is covered. addr must be a multiple of
 * FLASH_SECTOR. Waits until done. */
void flash_erase(uint32_t addr, uint32_t len);

/* Programs len bytes at addr (erased first), page by page. Waits until done. */
void flash_program(uint32_t addr, const uint8_t *data, uint32_t len);

/* CRC-32 (as crc32.h) of len bytes of flash from addr. */
uint32_t flash_crc(uint32_t addr, uint32_t len);

#endif
