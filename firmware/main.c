/* mod-player firmware: self-test, then serve commands from the PC
 * (docs/protocol.md) while playing. */
#include "crc32.h"
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
    default:
        reply_err("unknown command");
        break;
    }
}

int main(void)
{
    LEDS = 0x01;
    serial_puts("mod-player " VERSION "\r\n");
    uintptr_t bad = memtest(SDRAM_BASE, SDRAM_SIZE);
    if (bad) {
        serial_puts("sdram FAIL at ");
        serial_put_hex(bad);
        serial_puts("\r\n");
        for (;;)
            LEDS = (cycles() >> 24) & 1 ? 0xAA : 0x55;   /* blink code: SDRAM fault */
    }
    serial_puts("sdram ok\r\nready\r\n");
    player_init();
    for (;;) {
        if (serial_avail())
            command(serial_getc_timeout(0));
        player_poll();
    }
}
