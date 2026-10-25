/* The SoC's memory map, as seen by the firmware (see hw/rtl/soc.v). */
#ifndef HW_H
#define HW_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define UART_DATA   REG32(0x20000000u)  /* write: send a byte */
#define UART_STATUS REG32(0x20000004u)  /* see hw/rtl/soc.v */
#define UART_BUSY     0x1u
#define UART_RX_AVAIL 0x2u
#define UART_OVERRUN  0x4u

#define CPU_HZ      50000000u

#define SDRAM_BASE  0x10000000u
#define SDRAM_SIZE  (32u << 20)

#define PAULA_BASE  0x20000100u         /* registers: firmware/replayer/paula_port.h */

#define LEDS        REG32(0x20000200u)
#define SWITCHES    REG32(0x20000204u)  /* 1 = on */
#define SW_BLEND     0x1u               /* SW1: stereo blend */
#define SW_A500      0x2u               /* SW2: A500 filter */
#define SW_PLAY_ONCE 0x4u               /* SW3: stop at the end of the Song */
#define SW_DEV_MODE  0x8u               /* SW4: wait for the PC at power-on */

#define SPI_DATA    REG32(0x20000300u)  /* write: send a byte; read: the byte received */
#define SPI_CTRL    REG32(0x20000304u)  /* write bit 0: chip select on; read: */
#define SPI_BUSY      0x1u              /*   a transfer is in progress */
#define SPI_CS        0x2u              /*   chip select is on */

#endif
