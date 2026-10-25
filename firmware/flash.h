/* The board's SPI flash, through the SoC's SPI master (hw/rtl/spi_master.v). */
#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

/* Reads the JEDEC ID: manufacturer, memory type, capacity (the board's Macronix MX25L6406 is
 * c2 20 17). All 00 or all ff means no flash is answering. */
void flash_read_id(uint8_t id[3]);

#endif
