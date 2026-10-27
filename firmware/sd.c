#include "sd.h"
#include "hw.h"
#include "serial.h"

#define CMD0   0
#define CMD8   8
#define CMD16  16
#define CMD17  17
#define CMD55  55
#define CMD58  58
#define ACMD41 41

#define R1_IDLE    0x01u
#define R1_ILLEGAL 0x04u
#define TOKEN_DATA 0xFEu

#define SD_FAST 0u
#define SD_SLOW 2u               /* CTRL bit 1: SCK = clk / 128 */

static int sdhc, started;

static void wait_idle(void)
{
    while (SD_CTRL & SPI_BUSY)
        ;
}

static void select(int on, unsigned slow)
{
    wait_idle();
    SD_CTRL = (on ? 1u : 0u) | slow;
}

static uint8_t transfer(uint8_t out)
{
    wait_idle();
    SD_DATA = out;
    wait_idle();
    return (uint8_t)SD_DATA;
}

/* Chip select low, a command (6 bytes), then up to 8 bytes until the R1
 * response (high bit clear); 0xFF if none comes. Leaves the card selected. */
static uint8_t command(uint8_t index, uint32_t arg, uint8_t crc, unsigned slow)
{
    select(1, slow);
    transfer(0xFF);
    transfer((uint8_t)(0x40u | index));
    transfer((uint8_t)(arg >> 24));
    transfer((uint8_t)(arg >> 16));
    transfer((uint8_t)(arg >> 8));
    transfer((uint8_t)arg);
    transfer(crc);
    for (int i = 0; i < 8; i++) {
        uint8_t r = transfer(0xFF);
        if (!(r & 0x80u))
            return r;
    }
    return 0xFF;
}

/* Chip select high, and 8 more clocks so the card lets go of the data line. */
static void release(unsigned slow)
{
    select(0, slow);
    transfer(0xFF);
}

static uint32_t read_u32(void)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = v << 8 | transfer(0xFF);
    return v;
}

int sd_init(void)
{
    sdhc = 0;
    started = 0;
    /* at least 74 clocks with the card deselected and its data line high */
    select(0, SD_SLOW);
    for (int i = 0; i < 10; i++)
        transfer(0xFF);

    uint8_t r = 0xFF;
    for (int tries = 0; tries < 3 && r != R1_IDLE; tries++) {
        r = command(CMD0, 0, 0x95, SD_SLOW);
        release(SD_SLOW);
    }
    if (r == 0xFF)
        return SD_ERR_NO_CARD;
    if (r != R1_IDLE)
        return SD_ERR_UNSUPPORTED;

    int v2 = 0;
    r = command(CMD8, 0x1AA, 0x87, SD_SLOW);
    if (r == R1_IDLE) {
        uint32_t echo = read_u32();
        release(SD_SLOW);
        if ((echo & 0xFFFu) != 0x1AAu)
            return SD_ERR_UNSUPPORTED;
        v2 = 1;
    } else {
        release(SD_SLOW);
        if (!(r & R1_ILLEGAL))            /* a version 1 card says "illegal command" */
            return SD_ERR_UNSUPPORTED;
    }

    uint32_t start = cycles();
    for (;;) {                            /* ACMD41 until the card is ready: up to a second */
        command(CMD55, 0, 0x01, SD_SLOW);
        release(SD_SLOW);
        r = command(ACMD41, v2 ? 0x40000000u : 0, 0x01, SD_SLOW);
        release(SD_SLOW);
        if (r == 0)
            break;
        if (r != R1_IDLE)
            return SD_ERR_UNSUPPORTED;
        if (cycles() - start > CPU_HZ)
            return SD_ERR_NOT_READY;
    }

    if (v2) {
        r = command(CMD58, 0, 0x01, SD_SLOW);
        uint32_t ocr = read_u32();
        release(SD_SLOW);
        if (r != 0)
            return SD_ERR_UNSUPPORTED;
        sdhc = (ocr >> 30) & 1u;          /* CCS */
    }
    if (!sdhc) {
        r = command(CMD16, SD_BLOCK, 0x01, SD_SLOW);
        release(SD_SLOW);
        if (r != 0)
            return SD_ERR_UNSUPPORTED;
    }
    select(0, SD_FAST);                   /* started: the full 12.5 MHz from now on */
    started = 1;
    return SD_OK;
}

int sd_is_sdhc(void)
{
    return sdhc;
}

int sd_read_block(uint32_t lba, uint8_t *dst)
{
    if (!started)
        return SD_ERR_NOT_STARTED;
    uint8_t r = command(CMD17, sdhc ? lba : lba * SD_BLOCK, 0x01, SD_FAST);
    if (r != 0) {
        release(SD_FAST);
        return SD_ERR_READ;
    }
    uint32_t start = cycles();
    uint8_t token;
    while ((token = transfer(0xFF)) == 0xFF)      /* the card is fetching it: up to 100 ms */
        if (cycles() - start > CPU_HZ / 10) {
            release(SD_FAST);
            return SD_ERR_READ;
        }
    if (token != TOKEN_DATA) {                    /* an error token */
        release(SD_FAST);
        return SD_ERR_READ;
    }
    for (uint32_t i = 0; i < SD_BLOCK; i++)
        dst[i] = transfer(0xFF);
    transfer(0xFF);                               /* the CRC: not checked in SPI mode */
    transfer(0xFF);
    release(SD_FAST);
    return SD_OK;
}

const char *sd_error_text(int err)
{
    switch (err) {
    case SD_ERR_NO_CARD: return "no SD card";
    case SD_ERR_NOT_READY: return "SD card doesn't start up";
    case SD_ERR_UNSUPPORTED: return "SD card not supported";
    case SD_ERR_READ: return "SD read failed";
    case SD_ERR_NOT_STARTED: return "SD card not started";
    default: return "SD error";
    }
}
