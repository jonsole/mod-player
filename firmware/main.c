/* mod-player firmware: self-test, then serve commands from the PC
 * (docs/protocol.md) while playing. */
#include "console.h"
#include "crc32.h"
#include "flash.h"
#include "hw.h"
#include "memtest.h"
#include "mod.h"
#include "player.h"
#include "fat.h"
#include "sd.h"
#include "serial.h"
#include "status.h"
#include "version.h"

#define TIMEOUT_US 100000u
#define SONG ((uint8_t *)SDRAM_BASE)
#define SONG_MAX (SDRAM_SIZE - PLAYER_SILENCE_BYTES)

/* The card file playing (its first cluster), for SW3's next Song; 0: the
 * Song didn't come from the card. */
static uint32_t card_song;

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
    card_song = 0;
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

/* At power-on: play the Song saved in flash, if there is a good one
 * (docs/flash-layout.md). SW4 (developer mode) leaves it for the PC. */
static void play_saved_song(void)
{
    uint32_t header[4];
    flash_read(FLASH_SONG_ADDR, (uint8_t *)header, sizeof header);
    uint32_t size = header[1], crc = header[2];
    if (header[0] != FLASH_SONG_MAGIC || size == 0 || size > FLASH_SONG_SIZE - 16 || size > SONG_MAX)
        return;                                     /* nothing saved */
    if (SWITCHES & SW_DEV_MODE) {
        serial_puts("saved Song not played: SW4 is on\r\n");
        return;
    }
    if (flash_read(FLASH_SONG_ADDR + 16, SONG, size) != crc) {
        serial_puts("saved Song damaged: ignored\r\n");
        return;
    }
    const char *err = player_load(SONG, size);
    if (err) {
        serial_puts("saved Song not playable: ");
        serial_puts(err);
        serial_puts("\r\n");
        return;
    }
    player_play();
    serial_puts("saved Song: ");
    serial_puts(player_song()->name);
    serial_puts("\r\n");
}

/* D: start the SD card. */
static void cmd_sd_init(void)
{
    player_stop();
    int err = sd_init();
    if (err)
        reply_err(sd_error_text(err));
    else
        serial_puts(sd_is_sdhc() ? "OK sd sdhc\r\n" : "OK sd sdsc\r\n");
}

/* E: first block (u32), count (u32): the CRC-32 of those blocks of the
 * started card. */
static void cmd_sd_crc(void)
{
    static uint8_t block[SD_BLOCK];
    uint32_t lba, count;
    if (get_u32(&lba) < 0 || get_u32(&count) < 0) {
        reply_err("timeout");
        return;
    }
    player_stop();
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < count; i++) {
        int err = sd_read_block(lba + i, block);
        if (err) {
            reply_err(sd_error_text(err));
            return;
        }
        for (uint32_t j = 0; j < SD_BLOCK; j++)
            crc = crc32_update(crc, block[j]);
    }
    serial_puts("OK crc ");
    serial_put_hex(~crc);
    serial_puts("\r\n");
}

/* ---- the card's files (issue #26) ---------------------------------------------- */

/* A file name from the PC: one length byte, then the name. */
static int get_name(char *name, int max)
{
    int n = serial_getc_timeout(TIMEOUT_US);
    if (n < 0)
        return -1;
    for (int i = 0; i < n; i++) {
        int b = serial_getc_timeout(TIMEOUT_US);
        if (b < 0)
            return -1;
        if (i < max - 1)
            name[i] = (char)b;
    }
    name[n < max - 1 ? n : max - 1] = 0;
    return 0;
}

/* Starts the card and loads the file into SONG; its size in *size. */
static int load_from_card(const char *name, uint32_t *size)
{
    fat_file_t f;
    int err = fat_mount();
    if (!err)
        err = fat_find(name, &f);
    if (!err)
        err = fat_read(&f, SONG, SONG_MAX);
    if (!err) {
        *size = f.size;
        card_song = f.first_cluster;
    }
    return err;
}

/* SW3 on and the Song from the card has ended: the next Song file on the
 * card, passing over any that don't load (at most a lap of the directory). */
static void play_next_card_song(void)
{
    static char name[256];
    if (fat_mount())
        return;
    uint32_t after = card_song;
    for (int tries = 0; tries < 512; tries++) {
        fat_file_t f;
        if (fat_next_song(after, name, sizeof name, &f))
            return;
        after = f.first_cluster;
        if (after == card_song && tries > 0)
            return;                                 /* round the whole directory: nothing plays */
        if (fat_read(&f, SONG, SONG_MAX))
            continue;
        if (player_load(SONG, f.size)) {
            serial_puts("card Song not playable: ");
            serial_puts(name);
            serial_puts("\r\n");
            continue;
        }
        card_song = f.first_cluster;
        player_play();
        serial_puts("next Song: ");
        serial_puts(player_song()->name);
        serial_puts("\r\n");
        return;
    }
}

static int listed;

static void list_one(const char *name, uint32_t size)
{
    serial_puts("file ");
    serial_put_dec(size);
    serial_puts(" ");
    serial_puts(name);
    serial_puts("\r\n");
    listed++;
}

/* N: the files in the card's root directory. Stops the Song. */
static void cmd_card_list(void)
{
    player_stop();
    listed = 0;
    int err = fat_mount();
    if (!err)
        err = fat_list(list_one);
    if (err) {
        reply_err(fat_error_text(err));
        return;
    }
    serial_puts("OK ");
    serial_put_dec((uint32_t)listed);
    serial_puts(" files\r\n");
}

/* K: a name: the file's size and CRC-32, read from the card. Stops the Song. */
static void cmd_card_crc(void)
{
    char name[256];
    uint32_t size;
    if (get_name(name, sizeof name) < 0) {
        reply_err("timeout");
        return;
    }
    player_stop();
    int err = load_from_card(name, &size);
    if (err) {
        reply_err(fat_error_text(err));
        return;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; i++)
        crc = crc32_update(crc, SONG[i]);
    serial_puts("OK crc ");
    serial_put_hex(~crc);
    serial_puts(" size ");
    serial_put_dec(size);
    serial_puts("\r\n");
}

/* R: a name: load that Song from the card and play it. */
static void cmd_card_play(void)
{
    char name[256];
    uint32_t size;
    if (get_name(name, sizeof name) < 0) {
        reply_err("timeout");
        return;
    }
    player_stop();
    int err = load_from_card(name, &size);
    if (err)
        reply_err(fat_error_text(err));
    else
        start_song(size);
}

/* At power-on, with no saved Song: the card's default Song, if it has one. */
#define CARD_DEFAULT "default.mod"
static void play_card_default(void)
{
    uint32_t size;
    if (SWITCHES & SW_DEV_MODE)
        return;
    if (load_from_card(CARD_DEFAULT, &size))
        return;                                     /* no card, no FAT32, no such file */
    const char *err = player_load(SONG, size);
    if (err) {
        serial_puts("card Song not playable: ");
        serial_puts(err);
        serial_puts("\r\n");
        return;
    }
    player_play();
    serial_puts("card Song: ");
    serial_puts(player_song()->name);
    serial_puts("\r\n");
}

/* T: row, column, attribute, length (one byte each), then the text: puts
 * it on the HDMI text console. */
static void cmd_text(void)
{
    int arg[4];
    for (int i = 0; i < 4; i++)
        if ((arg[i] = serial_getc_timeout(TIMEOUT_US)) < 0) {
            reply_err("timeout");
            return;
        }
    for (int i = 0; i < arg[3]; i++) {
        int c = serial_getc_timeout(TIMEOUT_US);
        if (c < 0) {
            reply_err("timeout");
            return;
        }
        console_put(arg[0], arg[1] + i, (uint8_t)c, (uint8_t)arg[2]);
    }
    if (console_present())
        serial_puts("OK\r\n");
    else
        reply_err("no text console (not the HDMI build)");
}

/* The HDMI screen at power-on: what the self-tests found, and every
 * character and colour, to check the console by eye. */
static void draw_test_screen(int paula_ok)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!console_present())
        return;
    console_control(0);                 /* no scopes over the test screen */
    console_clear(ATTR_NORMAL);
    console_clear_row(0, 0, ATTR(WHITE, BLUE));
    console_print(0, 1, "mod-player " VERSION, ATTR(WHITE, BLUE));
    console_print(1, 1, "sdram ok", ATTR_NORMAL);
    console_print(1, 11, paula_ok ? "paula ok" : "paula FAIL: no tick", ATTR_NORMAL);
    for (int i = 0; i < 16; i++) {
        console_put(3, 4 + 2 * i, (uint8_t)hex[i], ATTR(DARK_GREY, BLACK));
        console_put(4 + i, 2, (uint8_t)hex[i], ATTR(DARK_GREY, BLACK));
    }
    for (int i = 0; i < 256; i++)
        console_put(4 + i / 16, 4 + 2 * (i % 16), (uint8_t)i, ATTR(WHITE, BLACK));
    for (int c = 0; c < 16; c++) {
        console_print(4 + c, 38, "   ", ATTR(BLACK, c));
        console_print(4 + c, 42, "Text", ATTR(c, BLACK));
        console_print(4 + c, 47, "Text", ATTR(c, c == WHITE ? BLACK : WHITE));
    }
}

/* B: back to the boot loader, which replies (ADR-0011). */
static void enter_boot_loader(void)
{
    player_stop();
    while (UART_STATUS & UART_BUSY)
        ;
    __asm__ volatile("jr %0" : : "r"(BOOT_FROM_FIRMWARE));
    __builtin_unreachable();
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
    case 'B':
        enter_boot_loader();
        break;
    case 'D':
        cmd_sd_init();
        break;
    case 'E':
        cmd_sd_crc();
        break;
    case 'N':
        cmd_card_list();
        break;
    case 'K':
        cmd_card_crc();
        break;
    case 'R':
        cmd_card_play();
        break;
    case 'T':
        cmd_text();
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
    int paula_ok = player_self_test();
    serial_puts(paula_ok ? "paula ok\r\n" : "paula FAIL: no tick\r\n");
    draw_test_screen(paula_ok);
    /* The power-on Song first, then "ready": bytes the PC sends while the
     * firmware is busy loading one are lost (the serial port holds one). */
    play_saved_song();
    if (!player_playing())
        play_card_default();
    serial_puts("ready\r\n");
    for (;;) {
        if (serial_avail())
            command(serial_getc_timeout(0));
        player_poll();
        if (player_take_finished() && (SWITCHES & SW_PLAY_ONCE) && card_song)
            play_next_card_song();
        status_update();
        if (!player_playing())          /* heartbeat on LED 0 while idle */
            LEDS = (cycles() >> 24) & 1;   /* about 3 blinks a second at 50 MHz */
    }
}
