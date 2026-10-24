/* mod-player firmware. */
#include "hw.h"

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

int main(void)
{
    LEDS = 0x01;
    uart_puts("mod-player ready\r\n");
    for (;;)
        ;
}
