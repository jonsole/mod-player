/* C model of Paula: the reference the Verilog Paula must match sample for
 * sample (ADR-0005). Time advances one PAL colour clock per step.
 * Register meanings are documented in paula_port.h.
 *
 * Output stage, applied every colour clock in this order, integer-only:
 *  1. Stereo blend (option): L' = (3L + R) >> 2, R' = (L + 3R) >> 2.
 *  2. A500 filter (option): one-pole low-pass, y += ((x << 15) - y) * 511 >> 16.
 *     Models the A500's RC filter (360 ohm, 0.1 uF): fc = 4421 Hz; the
 *     coefficient gives 4418 Hz at the colour-clock rate.
 *  3. LED filter (Song's E0x, unless the NO_LED option): two-pole low-pass,
 *     a state-variable filter with f = 359 / 65536 and damping
 *     99297 / 65536. Models the Sallen-Key filter (10k, 10k, 6800 pF,
 *     3900 pF): fc = 3091 Hz, Q = 0.660.
 * Filter state is held in Q15 (15 fractional bits); output is state >> 15.
 * The LED filter keeps its state while off, like the analog circuit.
 */
#ifndef PAULA_MODEL_H
#define PAULA_MODEL_H

#include <stdint.h>
#include "paula_port.h"

typedef struct {
    int32_t left, right;  /* sum of two voices, each sample (-128..127) x volume (0..64) */
} paula_frame_t;

typedef struct {
    /* written by the Replayer */
    uint32_t ptr;
    uint16_t len;
    uint32_t per;  /* already clamped: 113..65536 */
    uint8_t vol;   /* already clamped: 0..64 */
    /* internal state */
    int on;
    uint32_t location;      /* next byte to fetch */
    uint16_t len_left;      /* words left before the loop reload */
    int just_started;
    int8_t dat[2];          /* fetched word, first byte first */
    int bytes_left;         /* bytes of dat not yet played */
    uint32_t per_left;      /* colour clocks until the next sample step */
    int32_t out;            /* current sample x volume */
} paula_voice_t;

typedef struct {
    int32_t a500;            /* one-pole state, Q15 */
    int32_t low, band;       /* LED filter state, Q15 */
} paula_filter_t;

typedef struct {
    const uint8_t *mem;
    uint32_t mem_size;
    paula_voice_t voice[PAULA_VOICES];
    uint32_t options;
    int led;
    paula_filter_t filter[2]; /* left, right */
} paula_model_t;

/* Silences all voices. Paula reads samples from mem[0..mem_size); reads
 * outside it return 0. */
void paula_model_reset(paula_model_t *p, const uint8_t *mem, uint32_t mem_size);

void paula_model_write(paula_model_t *p, uint32_t reg, uint32_t value);

/* Returns the output during the current colour clock, then advances one. */
paula_frame_t paula_model_step(paula_model_t *p);

/* A Paula port whose writes go to this model. */
paula_port_t paula_model_port(paula_model_t *p);

#endif
