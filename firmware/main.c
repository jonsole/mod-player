/* mod-player firmware: self-test, then serve commands from the PC
 * (docs/protocol.md) while playing. */
#include "crc32.h"
#include "flash.h"
#include "hw.h"
#include "memtest.h"
#include "mod.h"
#include "player.h"
#include "serial.h"

#define VERSION "0.1"
#define TIMEOUT_US 100000u
#define SONG ((uint8_t *)SDRAM_BASE)
#define SONG_MAX (SDRAM_SIZE - PLAYER_SILENCE_BYTES)

static int get_u32(uint32_t *v)
{
    uint32_t x = 0;
    for (int i = 0; i < 4; i++) {
        int b = serial_getc_timeout(TIMEOUT_US);
        if (b < 0)
            return -1;
        x |= (uint32_t)b << (8 * i);
    }
    *v = x;
    return 0;
}

static void reply_err(const char *why)
{
    serial_puts("ERR ");
    serial_puts(why);
    serial_puts("\r\n");
}

/* A Song is in SDRAM: check it and start playing. */
static void start_song(uint32_t size)
{
    const char *err = player_load(SONG, size);
    if (err) {
        reply_err(err);
        return;
    }
    player_play();
    serial_puts("OK ");
    serial_puts(player_song()->name);
    serial_puts("\r\n");
}

static void cmd_load(void)
{
    uint32_t size, crc_sent;
    if (get_u32(&size) < 0) {
        reply_err("timeout");
        return;
    }
    player_stop();
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; i++) {
        int b = serial_getc_timeout(TIMEOUT_US);
        if (b < 0) {
            reply_err("timeout");
            return;
        }
        if (i < SONG_MAX)
            SONG[i] = (uint8_t)b;
        crc = crc32_update(crc, (uint8_t)b);
    }
    if (get_u32(&crc_sent) < 0) {
        reply_err("timeout");
        return;
    }
    if (size > SONG_MAX)
        reply_err("Song too large");
    else if (~crc != crc_sent)
        reply_err("checksum mismatch: transfer corrupted");
    else
        start_song(size);
}

static void cmd_info(void)
{
    const mod_song_t *s = player_song();
    if (!s) {
        reply_err("no Song loaded");
        return;
    }
    serial_puts("song ");
    serial_puts(s->name);
    serial_puts("\r\npositions ");
    serial_put_dec(s->length);
    serial_puts("\r\n");
    for (int i = 0; i < MOD_SAMPLES; i++) {
        if (s->sample[i].length == 0)
            continue;
        serial_puts("sample ");
        serial_put_dec((uint32_t)i + 1);
        serial_putc(' ');
        serial_puts(s->sample[i].name);
        serial_puts("\r\n");
    }
    serial_puts("OK\r\n");
}

static void put_hex_byte(uint8_t v)
{
    static const char digits[] = "0123456789abcdef";
    serial_putc(digits[v >> 4]);
    serial_putc(digits[v & 15]);
}

static void cmd_flash_id(void)
{
    uint8_t id[3];
    flash_read_id(id);
    serial_puts("OK flash");
    for (int i = 0; i < 3; i++) {
        serial_putc(' ');
        put_hex_byte(id[i]);
    }
    serial_puts("\r\n");
}

/* Flash writes are staged in the top half of SDRAM, clear of any Song. */
#define FLASH_STAGE ((uint8_t *)(SDRAM_BASE + SDRAM_SIZE / 2))

static void cmd_flash_write(void)
{
    uint32_t addr, size, crc_sent;
    if (get_u32(&addr) < 0 || get_u32(&size) < 0) {
        reply_err("timeout");
        return;
    }
    player_stop();
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; i++) {
        int b = serial_getc_timeout(TIMEOUT_US);
        if (b < 0) {
            reply_err("timeout");
            return;
        }
        if (i < FLASH_SIZE)
            FLASH_STAGE[i] = (uint8_t)b;
        crc = crc32_update(crc, (uint8_t)b);
    }
    if (get_u32(&crc_sent) < 0) {
        reply_err("timeout");
        return;
    }
    if (~crc != crc_sent)
        reply_err("checksum mismatch: transfer corrupted");
    else if (addr % FLASH_SECTOR != 0)
        reply_err("flash address must be a multiple of 4096");
    else if (size > FLASH_SIZE || addr > FLASH_SIZE - size)
        reply_err("past the end of the flash");
    else {
        flash_erase(addr, size);
        flash_program(addr, FLASH_STAGE, size);
        if (flash_crc(addr, size) != crc_sent)
            reply_err("flash doesn't read back as written");
        else
            serial_puts("OK written\r\n");
    }
}

static void cmd_flash_crc(void)
{
    uint32_t addr, size;
    if (get_u32(&addr) < 0 || get_u32(&size) < 0) {
        reply_err("timeout");
        return;
    }
    if (size > FLASH_SIZE || addr > FLASH_SIZE - size) {
        reply_err("past the end of the flash");
        return;
    }
    serial_puts("OK crc ");
    serial_put_hex(flash_crc(addr, size));
    serial_puts("\r\n");
}

static void command(int c)
{
    uint32_t size;
    switch (c) {
    case 'V':
        serial_puts("OK mod-player " VERSION "\r\n");
        break;
    case 'L':
        cmd_load();
        break;
    case 'M':
        if (get_u32(&size) < 0)
            reply_err("timeout");
        else if (size > SONG_MAX)
            reply_err("Song too large");
        else {
            player_stop();
            start_song(size);
        }
        break;
    case 'P':
        if (!player_song()) reply_err("no Song loaded");
        else { player_play(); serial_puts("OK\r\n"); }
        break;
    case 'S':
        player_stop();
        serial_puts("OK\r\n");
        break;
    case 'U':
        serial_puts(player_pause_toggle() ? "OK paused\r\n" : "OK playing\r\n");
        break;
    case 'I':
        cmd_info();
        break;
    case 'F':
        cmd_flash_id();
        break;
    case 'W':
        cmd_flash_write();
        break;
    case 'C':
        cmd_flash_crc();
        break;
    default:
        reply_err("unknown command");
        break;
    }
}

int main(void)
{
    LEDS = 0x01;
    serial_puts("mod-player " VERSION "\r\n");
    struct memtest_fail fail;
    if (memtest(SDRAM_BASE, SDRAM_SIZE, &fail)) {
        serial_puts("sdram FAIL at ");
        serial_put_hex(fail.addr);
        serial_puts(" expected ");
        serial_put_hex(fail.expected);
        serial_puts(" got ");
        serial_put_hex(fail.got);
        serial_puts("\r\n");
        for (;;)
            LEDS = (cycles() >> 24) & 1 ? 0xAA : 0x55;   /* blink code: SDRAM fault */
    }
    serial_puts("sdram ok\r\n");
    player_init();
    serial_puts(player_self_test() ? "paula ok\r\n" : "paula FAIL: no tick\r\n");
    serial_puts("ready\r\n");
    for (;;) {
        if (serial_avail())
            command(serial_getc_timeout(0));
        player_poll();
        if (!player_playing())          /* heartbeat on LED 0 while idle */
            LEDS = (cycles() >> 24) & 1;   /* about 3 blinks a second at 50 MHz */
    }
}
