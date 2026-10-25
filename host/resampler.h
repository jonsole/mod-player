/* C model of the HDMI Resampler (ADR-0010): turns Paula's frames, one per
 * colour clock (3 546 895 Hz), into 48 kHz stereo for HDMI. The Verilog
 * (hw/rtl/resampler.v) must match it output for output. Integer-only.
 *
 * Stage 1, CIC: 4th-order cascaded integrator-comb, decimating by 16 to
 * 221 681 Hz. Integrators and combs wrap at 32 bits (as CICs may); the comb
 * output is x * 2^16, kept as c = out >> 14 (18 bits, x * 4). Each frame
 * updates the integrators last stage first, from the others' old values,
 * as registers would. The 16th frame of each group (count 15) is the one
 * whose integrator value goes through the combs.
 *
 * Stage 2, polyphase FIR: 128 taps from a Kaiser-windowed sinc table at 32
 * points per intermediate sample (resampler_coefs.h). Each output runs the
 * FIR at the two table phases either side of its position and interpolates
 * linearly between the two results. Every phase sums to exactly 2^18, so
 * DC passes exactly. Output k sits k * 709379/153600 intermediate samples after
 * the first: the position is kept exactly as (gap, phase, frac), where phase
 * counts 1/32 samples and frac 1/4800 of those, so the rate is exactly
 * 48 kHz with no drift. For each output:
 *   w      = frac * 3495 >> 12                    (0..4095)
 *   n_j    = j * 32 + phase
 *   acc_a  = sum over j = 0..127 of c[latest - j] * h[n_j]
 *   acc_b  = sum over j = 0..127 of c[latest - j] * h[n_j + 1]
 *   mixed  = acc_a * 4096 + (acc_b - acc_a) * w
 *   out    = saturate24((mixed + 2^22) >> 23)       (x * 512)
 * then the position advances: frac += 3779 (carry at 4800 into phase),
 * phase += 19 (carry at 32 into the next output's gap of 3 or 4 skipped
 * intermediate samples).
 */
#ifndef RESAMPLER_H
#define RESAMPLER_H

#include <stdint.h>

#define RESAMPLER_CIC_R 16
#define RESAMPLER_HISTORY 128

typedef struct {
    uint32_t integ[2][4];      /* CIC integrators, per channel */
    uint32_t comb[2][4];       /* CIC comb delays */
    int count;                 /* frames into the current decimation group */
    int32_t hist[2][RESAMPLER_HISTORY]; /* intermediate samples, ring */
    int head;                  /* where the latest intermediate sample is */
    int gap;                   /* intermediate samples to skip before the next output */
    int phase;                 /* 0..31 */
    int frac;                  /* 0..4799 */
} resampler_t;

void resampler_reset(resampler_t *rs);

/* One Paula frame in (each channel as Paula's 16-bit output). Returns 1
 * and sets out[0] (left), out[1] (right), 24-bit signed, when a 48 kHz
 * output is due; otherwise 0. */
int resampler_push(resampler_t *rs, int32_t left, int32_t right, int32_t out[2]);

/* The prototype coefficient h[n], n = 0..4096 (symmetric, zero at the ends). */
int32_t resampler_coef(int n);

#endif
