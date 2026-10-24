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
 *
 * PAULA_INVERT: flip every bit of the byte at this address in Paula's memory
 * (invert loop, EFx). It goes through Paula rather than the CPU writing
 * memory, so it happens at the same moment as the tick's other writes.
 *
 * On the board, writes to all the registers above are queued and applied
 * together at the next tick boundary (ADR-0009); the PC model applies them
 * at once, which is the same thing seen from Paula's side.
 *
 * Board-only registers (not queued):
 *   PAULA_TICK_LEN  colour clocks in the next tick; loaded at each boundary
 *   PAULA_STATUS    bit 0: tick boundary passed (write 1 to clear);
 *                   bit 1: write queue overflowed; bit 2: engine fell behind;
 *                   bit 3: running
 *   PAULA_CONTROL   bit 0 (RUN): 1 starts ticking, with the first boundary at
 *                   the next colour clock; 0 pauses (queued writes are kept).
 *                   bit 1 (FLUSH): discard queued writes.
 *
 * PAULA_OPTIONS: output options set by the player, not the Song:
 *   PAULA_OPT_BLEND   mix a quarter of each side into the other (board SW1)
 *   PAULA_OPT_A500    the Amiga 500's fixed 4421 Hz low-pass filter (SW2)
 *   PAULA_OPT_NO_LED  ignore PAULA_LED_FILTER
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
#define PAULA_OPTIONS 0x48u
#define PAULA_OPT_BLEND 0x1u
#define PAULA_OPT_A500 0x2u
#define PAULA_OPT_NO_LED 0x4u
#define PAULA_INVERT 0x4Cu
#define PAULA_TICK_LEN 0x50u
#define PAULA_STATUS 0x54u
#define PAULA_STATUS_TICK 0x1u
#define PAULA_STATUS_QUEUE_OVERFLOW 0x2u
#define PAULA_STATUS_BEHIND 0x4u
#define PAULA_STATUS_RUNNING 0x8u
#define PAULA_CONTROL 0x58u
#define PAULA_CONTROL_RUN 0x1u
#define PAULA_CONTROL_FLUSH 0x2u

/* The PAL Amiga colour clock: Paula's time unit. */
#define PAULA_CCK_HZ 3546895u

typedef struct paula_port {
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
    void *ctx;
} paula_port_t;

#endif
