#ifndef SERIAL_H
#define SERIAL_H
#include <stdint.h>

void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex(uint32_t v);
void serial_put_dec(uint32_t v);
int serial_avail(void);
/* Returns the next byte, or -1 after timeout_us without one. */
int serial_getc_timeout(uint32_t timeout_us);
uint32_t cycles(void);

#endif
