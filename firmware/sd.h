/* The board's microSD card in SPI mode (issue #25), through the SoC's second
 * SPI master (hw/rtl/spi_master.v). Cards are read in 512-byte blocks. */
#ifndef SD_H
#define SD_H

#include <stdint.h>

#define SD_BLOCK 512u

enum {
    SD_OK = 0,
    SD_ERR_NO_CARD,        /* nothing answers CMD0 */
    SD_ERR_NOT_READY,      /* the card doesn't finish starting up */
    SD_ERR_UNSUPPORTED,    /* it answers, but not like an SD card in SPI mode */
    SD_ERR_READ,           /* a block read failed: past the end, or no data */
    SD_ERR_NOT_STARTED,    /* sd_init() hasn't succeeded: nothing is sent to the card */
};

/* Starts the card (74 clocks, then CMD0, CMD8, ACMD41, CMD58 at 390 kHz, then
 * the full 12.5 MHz). Returns SD_OK or an error. */
int sd_init(void);
/* 1 for an SDHC/SDXC card (block addressed), 0 for a standard-capacity one. */
int sd_is_sdhc(void);
/* Reads block `lba` into dst (512 bytes). Returns SD_OK or SD_ERR_READ. */
int sd_read_block(uint32_t lba, uint8_t *dst);
const char *sd_error_text(int err);

#endif
