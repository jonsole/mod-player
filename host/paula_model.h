/* C model of Paula: the reference the Verilog Paula must match sample for
 * sample (ADR-0005). Time advances one PAL colour clock per step.
 * Register meanings are documented in paula_port.h.
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
    const uint8_t *mem;
    uint32_t mem_size;
    paula_voice_t voice[PAULA_VOICES];
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
