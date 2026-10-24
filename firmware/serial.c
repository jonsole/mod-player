#include "serial.h"
#include "hw.h"

uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("rdcycle %0" : "=r"(c));
    return c;
}

void serial_putc(char c)
{
    while (UART_STATUS & UART_BUSY)
        ;
    UART_DATA = (uint8_t)c;
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

void serial_put_hex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        serial_putc("0123456789abcdef"[(v >> i) & 0xF]);
}

void serial_put_dec(uint32_t v)
{
    char buf[11];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        serial_putc(buf[--n]);
}

int serial_avail(void)
{
    return (UART_STATUS & UART_RX_AVAIL) != 0;
}

int serial_getc_timeout(uint32_t timeout_us)
{
    uint32_t start = cycles(), limit = timeout_us * (CPU_HZ / 1000000u);
    while (!serial_avail())
        if (cycles() - start > limit)
            return -1;
    return (int)(UART_DATA & 0xFF);
}
