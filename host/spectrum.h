/* The spectrum analyser (issue #24, ADR-0014): the C model that the Verilog
 * (hw/rtl/spectrum.v) must match bar for bar. Integer only.
 *
 * Input: the HDMI Resampler's 48 kHz output (24-bit, Paula's value x 512),
 * left and right mixed: x = (left + right) >> 8 (18-bit).
 *
 * Once a video frame:
 *  1. The last 1024 samples, oldest first, times a Hann window (Q15,
 *     rounded), go into the FFT in bit-reversed order.
 *  2. A radix-2 decimation-in-time FFT, 10 stages of 512 butterflies. Each
 *     butterfly: t = b * W rounded from Q17 (W = cos - i sin, read from a
 *     quarter-wave sine table), then a' = (a + t + 1) >> 1 and
 *     b' = (a - t + 1) >> 1, so values stay 18-bit (the result is the DFT
 *     / 1024).
 *  3. Each bar's level is the loudest of its bins (spectrum_tables.h), in
 *     eighths of an octave of power: L = 8 * msb(P) + the next 3 bits of P,
 *     P = re^2 + im^2 (0 for P = 0). Its height is (L - SPECTRUM_FLOOR)
 *     x SPECTRUM_SCALE / 8, clamped to 0..SPECTRUM_HEIGHT pixels: one pixel
 *     is 0.2 dB, and the bars span 36 dB (the owner found 68 dB lit every
 *     bar with quiet background energy).
 *  4. The bar shown rises at once and falls SPECTRUM_FALL pixels a frame;
 *     its peak marker holds for SPECTRUM_HOLD frames, then falls a pixel a
 *     frame.
 */
#ifndef SPECTRUM_H
#define SPECTRUM_H
#include <stdint.h>
#include "spectrum_tables.h"

#define SPECTRUM_HEIGHT 180
#define SPECTRUM_FLOOR  100
#define SPECTRUM_SCALE  15             /* eighths of a pixel per level */
#define SPECTRUM_FALL   4
#define SPECTRUM_HOLD   30

typedef struct {
    int32_t ring[SPECTRUM_N];    /* the last N samples */
    unsigned next;               /* where the next sample goes: the oldest */
    int32_t re[SPECTRUM_N], im[SPECTRUM_N];   /* the last frame's FFT */
    uint16_t level[SPECTRUM_BARS];            /* and each bar's level */
    uint8_t bar[SPECTRUM_BARS], peak[SPECTRUM_BARS], hold[SPECTRUM_BARS];
} spectrum_t;

void spectrum_reset(spectrum_t *s);
/* One 48 kHz sample from the Resampler: 24-bit left and right. */
void spectrum_push(spectrum_t *s, int32_t left, int32_t right);
/* One video frame: analyse the last N samples and update the bars. */
void spectrum_frame(spectrum_t *s);
/* The level of power P (exposed for tests). */
unsigned spectrum_log(uint64_t p);

#endif
