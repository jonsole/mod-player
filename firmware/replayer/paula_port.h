/* The Paula port: the only way the Replayer talks to Paula.
 *
 * Register offsets are the same on the board (memory-mapped, relative to
 * Paula's base address) and in the PC model. Every value is written as a
 * 32-bit word.
 *
 * Per voice v (0..3), registers are at PAULA_VOICE(v) + one of:
 *   PAULA_PTR  sample address in Paula's memory space (byte address, even)
 *   PAULA_LEN  length in 16-bit words; 0 means 65536 words
 *   PAULA_PER  period in colour clocks per sample; 0 means 65536, below 113 acts as 113
 *   PAULA_VOL  volume 0..64; bit 6 set means 64, higher bits ignored
 *
 * PTR and LEN are latched when a voice starts and re-latched every time the
 * voice reaches the end of its sample, so the Replayer can set the loop
 * straight after starting a note (as on the Amiga). PER and VOL take effect at
 * the voice's next sample step.
 *
 * PAULA_DMACON works like the Amiga's: bit 15 set means "start the voices in
 * bits 0..3", bit 15 clear means "stop them".
 *
 * PAULA_LED_FILTER: 1 turns the Amiga "LED" low-pass filter on, 0 off
 * (effect E0x; on the Amiga this lived in a CIA register).
 */
#ifndef PAULA_PORT_H
#define PAULA_PORT_H

#include <stdint.h>

#define PAULA_VOICES 4

#define PAULA_VOICE(v) ((uint32_t)(v) * 0x10u)
#define PAULA_PTR 0x0u
#define PAULA_LEN 0x4u
#define PAULA_PER 0x8u
#define PAULA_VOL 0xCu
#define PAULA_DMACON 0x40u
#define PAULA_DMACON_SET 0x8000u
#define PAULA_LED_FILTER 0x44u

/* The PAL Amiga colour clock: Paula's time unit. */
#define PAULA_CCK_HZ 3546895u

typedef struct paula_port {
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
    void *ctx;
} paula_port_t;

#endif
