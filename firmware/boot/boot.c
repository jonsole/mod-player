/* The boot loader (ADR-0006, ADR-0011). The FPGA image's RAM holds only
 * this, at BOOT_BASE; it puts the firmware at address 0 and runs it.
 *
 * At power-on it waits WINDOW_US for the PC. If no byte arrives it runs the
 * firmware saved in flash (docs/flash-layout.md); without good firmware there
 * it waits for the PC. The PC sends firmware with the X command
 * (docs/protocol.md). The firmware's B command comes back here.
 *
 * Built with -fno-delete-null-pointer-checks: the firmware lives at
 * address 0, so writing through a null pointer is what we mean. */
#include "crc32.h"
#include "flash.h"
#include "hw.h"
#include "serial.h"

#define VERSION    "0.1"
#define TIMEOUT_US 100000u      /* between bytes of a command, as the firmware */
#define WINDOW_US  200000u      /* for the PC at power-on */
#define FIRMWARE   ((uint8_t *)0)

static void reply_err(const char *why)
{
    serial_puts("ERR ");
    serial_puts(why);
    serial_puts("\r\n");
}

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

static void __attribute__((noreturn)) run_firmware(void)
{
    while (UART_STATUS & UART_BUSY)
        ;
    __asm__ volatile("jr zero");
    __builtin_unreachable();
}

/* Runs the firmware in flash; returns only if there is no good one. */
static void run_flash_firmware(void)
{
    uint32_t header[4];
    flash_read(FLASH_FW_ADDR, (uint8_t *)header, sizeof header);
    uint32_t size = header[1], crc = header[2];
    if (header[0] != FLASH_FW_MAGIC || size == 0 || size > BOOT_BASE) {
        serial_puts("no firmware in flash: waiting for serial\r\n");
        return;
    }
    if (flash_read(FLASH_FW_ADDR + 16, FIRMWARE, size) != crc) {
        serial_puts("firmware in flash damaged: waiting for serial\r\n");
        return;
    }
    serial_puts("firmware from flash\r\n");
    run_firmware();
}

/* X: length, the firmware image, CRC-32. Received straight into place;
 * checked before it runs. */
static void cmd_run(void)
{
    uint32_t size, crc_sent;
    if (get_u32(&size) < 0) {
        reply_err("timeout");
        return;
    }
    for (uint32_t i = 0; i < size; i++) {
        int b = serial_getc_timeout(TIMEOUT_US);
        if (b < 0) {
            reply_err("timeout");
            return;
        }
        if (i < BOOT_BASE)
            FIRMWARE[i] = (uint8_t)b;
    }
    if (get_u32(&crc_sent) < 0) {
        reply_err("timeout");
        return;
    }
    if (size == 0 || size > BOOT_BASE) {
        reply_err("firmware too large for its RAM, or empty");
        return;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; i++)
        crc = crc32_update(crc, FIRMWARE[i]);
    if (~crc != crc_sent) {
        reply_err("checksum mismatch: transfer corrupted");
        return;
    }
    serial_puts("OK running\r\n");
    run_firmware();
}

void boot_main(int from_firmware)
{
    LEDS = 0x80;
    if (from_firmware) {
        serial_puts("OK boot loader " VERSION "\r\n");
    } else {
        serial_puts("boot loader " VERSION "\r\n");
        uint32_t start = cycles();
        while (!serial_avail() && cycles() - start < WINDOW_US * (CPU_HZ / 1000000u))
            ;
        if (!serial_avail())
            run_flash_firmware();
    }
    for (;;) {
        while (!serial_avail())
            ;
        switch (serial_getc_timeout(0)) {
        case 'V':
        case 'B':
            serial_puts("OK boot loader " VERSION "\r\n");
            break;
        case 'X':
            cmd_run();
            break;
        default:
            reply_err("unknown command");
            break;
        }
    }
}
