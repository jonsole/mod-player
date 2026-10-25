/* C model of the HDMI Resampler; see resampler.h for the arithmetic. */
#include "resampler.h"

#include <string.h>

#include "resampler_coefs.h"

#define FULL (RESAMPLER_TAPS * RESAMPLER_PHASES)   /* 4096 */

void resampler_reset(resampler_t *rs)
{
    memset(rs, 0, sizeof *rs);
}

int32_t resampler_coef(int n)
{
    if (n <= 0 || n >= FULL) return 0;
    if (n > RESAMPLER_HALF) n = FULL - n;
    return resampler_coefs[n - 1];
}

static int32_t saturate24(int64_t v)
{
    if (v > 8388607) return 8388607;
    if (v < -8388608) return -8388608;
    return (int32_t)v;
}

/* CIC stage for one channel: integrate; at the end of a group, comb.
 * Returns the comb output when `dump` is set. */
static int32_t cic(resampler_t *rs, int ch, int32_t x, int dump)
{
    uint32_t *in = rs->integ[ch];
    /* last stage first: each adds the previous stage's old value, as
     * registers do in hardware (one adder per stage per clock) */
    in[3] += in[2];
    in[2] += in[1];
    in[1] += in[0];
    in[0] += (uint32_t)x;
    if (!dump) return 0;
    uint32_t v = in[3];
    for (int s = 0; s < 4; s++) {
        uint32_t d = v - rs->comb[ch][s];
        rs->comb[ch][s] = v;
        v = d;
    }
    return (int32_t)v >> 14;  /* arithmetic shift: x * 4 */
}

int resampler_push(resampler_t *rs, int32_t left, int32_t right, int32_t out[2])
{
    int dump = rs->count == RESAMPLER_CIC_R - 1;
    int32_t c0 = cic(rs, 0, left, dump);
    int32_t c1 = cic(rs, 1, right, dump);
    rs->count = dump ? 0 : rs->count + 1;
    if (!dump) return 0;

    rs->head = (rs->head + 1) % RESAMPLER_HISTORY;
    rs->hist[0][rs->head] = c0;
    rs->hist[1][rs->head] = c1;
    if (rs->gap > 0) {
        rs->gap--;
        return 0;
    }

    int64_t w = (rs->frac * 3495) >> 12;
    for (int ch = 0; ch < 2; ch++) {
        int64_t acc_a = 0, acc_b = 0;
        for (int j = 0; j < RESAMPLER_TAPS; j++) {
            int n = j * RESAMPLER_PHASES + rs->phase;
            int h = (rs->head - j + RESAMPLER_HISTORY) % RESAMPLER_HISTORY;
            acc_a += (int64_t)rs->hist[ch][h] * resampler_coef(n);
            acc_b += (int64_t)rs->hist[ch][h] * resampler_coef(n + 1);
        }
        int64_t mixed = acc_a * 4096 + (acc_b - acc_a) * w;
        out[ch] = saturate24((mixed + ((int64_t)1 << 22)) >> 23);
    }

    rs->frac += 3779;
    if (rs->frac >= 4800) {
        rs->frac -= 4800;
        rs->phase++;
    }
    rs->phase += 19;
    rs->gap = 3;
    if (rs->phase >= RESAMPLER_PHASES) {
        rs->phase -= RESAMPLER_PHASES;
        rs->gap = 4;
    }
    return 1;
}
