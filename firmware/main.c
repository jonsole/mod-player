/* mod-player firmware. */
#include "hw.h"
#include "memtest.h"

static void uart_putc(char c)
{
    while (UART_STATUS & UART_BUSY)
        ;
    UART_DATA = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void uart_hex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        uart_putc("0123456789abcdef"[(v >> i) & 0xF]);
}

int main(void)
{
    LEDS = 0x01;
    uart_puts("mod-player ready\r\n");
    uintptr_t bad = memtest(SDRAM_BASE, SDRAM_SIZE);
    if (bad) {
        uart_puts("sdram FAIL at ");
        uart_hex(bad);
        uart_puts("\r\n");
        LEDS = 0xAA;
    } else {
        uart_puts("sdram ok\r\n");
        LEDS = 0x03;
    }
    for (;;)
        ;
}
