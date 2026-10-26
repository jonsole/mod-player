#include "spectrum.h"
#include <string.h>

void spectrum_reset(spectrum_t *s)
{
    memset(s, 0, sizeof *s);
}

void spectrum_push(spectrum_t *s, int32_t left, int32_t right)
{
    s->ring[s->next] = (left + right) >> 8;
    s->next = (s->next + 1) % SPECTRUM_N;
}

static unsigned bit_reverse(unsigned n)
{
    unsigned r = 0;
    for (int i = 0; i < 10; i++)
        r |= ((n >> i) & 1) << (9 - i);
    return r;
}

/* sin and cos of 2 pi k / 1024 for k = 0..511, Q17, from the quarter wave */
static int32_t sine_of(unsigned k)
{
    return k <= 256 ? spectrum_sine[k] : spectrum_sine[512 - k];
}

static int32_t cosine_of(unsigned k)
{
    return k <= 256 ? spectrum_sine[256 - k] : -spectrum_sine[k - 256];
}

unsigned spectrum_log(uint64_t p)
{
    if (p == 0)
        return 0;
    int msb = 63;
    while (!((p >> msb) & 1))
        msb--;
    unsigned frac = msb >= 3 ? (unsigned)(p >> (msb - 3)) & 7 : (unsigned)(p << (3 - msb)) & 7;
    return (unsigned)msb * 8 + frac;
}

void spectrum_frame(spectrum_t *s)
{
    /* 1. window, in bit-reversed order */
    for (unsigned n = 0; n < SPECTRUM_N; n++) {
        int64_t x = s->ring[(s->next + n) % SPECTRUM_N];
        s->re[bit_reverse(n)] = (int32_t)((x * spectrum_window[n] + (1 << 14)) >> 15);
        s->im[bit_reverse(n)] = 0;
    }
    /* 2. the FFT */
    for (int stage = 0; stage < 10; stage++) {
        unsigned half = 1u << stage;
        for (unsigned j = 0; j < SPECTRUM_N / 2; j++) {
            unsigned pos = j & (half - 1);
            unsigned a = ((j >> stage) << (stage + 1)) | pos, b = a + half;
            unsigned k = pos << (9 - stage);
            int64_t wr = cosine_of(k), wi = -sine_of(k);
            int64_t br = s->re[b], bi = s->im[b];
            int32_t tr = (int32_t)((br * wr - bi * wi + (1 << 16)) >> 17);
            int32_t ti = (int32_t)((br * wi + bi * wr + (1 << 16)) >> 17);
            int32_t ar = s->re[a], ai = s->im[a];
            s->re[a] = (ar + tr + 1) >> 1;
            s->im[a] = (ai + ti + 1) >> 1;
            s->re[b] = (ar - tr + 1) >> 1;
            s->im[b] = (ai - ti + 1) >> 1;
        }
    }
    /* 3, 4. the bars */
    for (int bar = 0; bar < SPECTRUM_BARS; bar++) {
        unsigned level = 0;
        for (unsigned bin = spectrum_bar_lo[bar]; bin <= spectrum_bar_hi[bar]; bin++) {
            int64_t re = s->re[bin], im = s->im[bin];
            unsigned l = spectrum_log((uint64_t)(re * re + im * im));
            if (l > level)
                level = l;
        }
        s->level[bar] = (uint16_t)level;
        unsigned h = level <= SPECTRUM_FLOOR ? 0 : level - SPECTRUM_FLOOR;
        if (h > SPECTRUM_HEIGHT)
            h = SPECTRUM_HEIGHT;
        unsigned fallen = s->bar[bar] > SPECTRUM_FALL ? s->bar[bar] - SPECTRUM_FALL : 0;
        s->bar[bar] = (uint8_t)(h > fallen ? h : fallen);
        if (s->bar[bar] >= s->peak[bar]) {
            s->peak[bar] = s->bar[bar];
            s->hold[bar] = SPECTRUM_HOLD;
        } else if (s->hold[bar]) {
            s->hold[bar]--;
        } else {
            s->peak[bar]--;
        }
    }
}
