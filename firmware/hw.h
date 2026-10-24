/* The SoC's memory map, as seen by the firmware (see hw/rtl/soc.v). */
#ifndef HW_H
#define HW_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define UART_DATA   REG32(0x20000000u)  /* write: send a byte */
#define UART_STATUS REG32(0x20000004u)  /* bit 0: still sending */
#define UART_BUSY   0x1u

#define SDRAM_BASE  0x10000000u
#define SDRAM_SIZE  (32u << 20)

#define LEDS        REG32(0x20000200u)
#define SWITCHES    REG32(0x20000204u)

#endif
