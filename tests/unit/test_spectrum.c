/* The spectrum analyser's C model (host/spectrum.c): these tests pin what
 * it shows; the Verilog must then match the model exactly
 * (hw/sim/spectrum_tb.cpp). */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "spectrum.h"

#define PI 3.14159265358979323846

static spectrum_t sp;

/* n samples of a sine at hz with amplitude amp (in the 18-bit mix), on
 * both channels */
static void push_sine(double hz, double amp, int n)
{
    static long t;
    for (int i = 0; i < n; i++, t++) {
        int32_t x = (int32_t)lround(amp * sin(2 * PI * hz * (double)t / 48000.0));
        spectrum_push(&sp, x * 128, x * 128);           /* (l + r) >> 8 = x */
    }
}

static void push_silence(int n)
{
    for (int i = 0; i < n; i++)
        spectrum_push(&sp, 0, 0);
}

static int loudest_bar(void)
{
    int best = 0;
    for (int b = 1; b < SPECTRUM_BARS; b++)
        if (sp.level[b] > sp.level[best])
            best = b;
    return best;
}

/* the middle of a bar's bins, in Hz */
static double bar_hz(int b)
{
    return (spectrum_bar_lo[b] + spectrum_bar_hi[b]) / 2.0 * 48000.0 / SPECTRUM_N;
}

static void test_the_log_is_eighths_of_an_octave(void)
{
    CHECK_EQ(spectrum_log(0), 0);
    CHECK_EQ(spectrum_log(1), 0);
    CHECK_EQ(spectrum_log(2), 8);
    CHECK_EQ(spectrum_log(3), 12);
    CHECK_EQ(spectrum_log(8), 24);
    CHECK_EQ(spectrum_log(15), 31);
    CHECK_EQ(spectrum_log(1ULL << 34), 272);
    CHECK_EQ(spectrum_log((1ULL << 35) - 1), 279);         /* the largest power: bit 34 */
}

static void test_silence_shows_nothing(void)
{
    spectrum_reset(&sp);
    push_silence(2048);
    spectrum_frame(&sp);
    for (int b = 0; b < SPECTRUM_BARS; b++) {
        CHECK_EQ(sp.bar[b], 0);
        CHECK_EQ(sp.peak[b], 0);
    }
}

static void test_a_tone_lights_its_own_bar(void)
{
    int bars[] = {0, 3, 8, 10, 14, 17, 20, 23};
    for (unsigned i = 0; i < sizeof bars / sizeof bars[0]; i++) {
        spectrum_reset(&sp);
        push_sine(bar_hz(bars[i]), 20000, 1024);
        spectrum_frame(&sp);
        CHECK_EQ(loudest_bar(), bars[i]);
        /* and bars well away from it are far lower (the Hann window's leakage) */
        for (int b = 0; b < SPECTRUM_BARS; b++)
            if ((b < bars[i] - 3 || b > bars[i] + 3) && sp.bar[b] + 60 > sp.bar[bars[i]])
                CHECK_EQ(sp.bar[b], 0);
    }
}

static void test_a_full_scale_tone_nearly_fills_the_height(void)
{
    spectrum_reset(&sp);
    push_sine(bar_hz(15), 65000, 1024);
    spectrum_frame(&sp);
    CHECK(sp.bar[15] >= 170);
    CHECK(sp.bar[15] <= SPECTRUM_HEIGHT);
}

static void test_40_dB_quieter_is_106_pixels_lower(void)
{
    spectrum_reset(&sp);
    push_sine(bar_hz(15), 30000, 1024);
    spectrum_frame(&sp);
    int loud = sp.level[15];
    spectrum_reset(&sp);
    push_sine(bar_hz(15), 300, 1024);
    spectrum_frame(&sp);
    int quiet = sp.level[15];
    CHECK(abs(loud - quiet - 106) <= 4);           /* 40 dB / 0.376 dB a step */
}

static void test_bars_fall_and_peaks_hold_then_fall(void)
{
    spectrum_reset(&sp);
    push_sine(bar_hz(20), 30000, 1024);
    spectrum_frame(&sp);
    int top = sp.bar[20];
    CHECK(top > 100);
    CHECK_EQ(sp.peak[20], top);
    push_silence(1024);
    for (int f = 1; f <= SPECTRUM_HOLD; f++) {
        spectrum_frame(&sp);
        CHECK_EQ(sp.bar[20], top - SPECTRUM_FALL * f > 0 ? top - SPECTRUM_FALL * f : 0);
        CHECK_EQ(sp.peak[20], top);                   /* held */
    }
    for (int f = 1; f <= 10; f++) {
        spectrum_frame(&sp);
        CHECK_EQ(sp.peak[20], top - f);               /* then a pixel a frame */
    }
}

/* The FFT against a plain DFT (in double, / 1024) of the windowed samples */
static void test_the_fft_is_a_dft_divided_by_1024(void)
{
    spectrum_reset(&sp);
    uint32_t rng = 1;
    double x[SPECTRUM_N];
    for (int n = 0; n < SPECTRUM_N; n++) {
        rng = rng * 1103515245u + 12345u;
        int32_t v = (int32_t)((rng >> 8) % 120001) - 60000;
        spectrum_push(&sp, v * 128, v * 128);
        x[n] = (double)v * spectrum_window[n] / 32768.0;
    }
    spectrum_frame(&sp);
    double worst = 0;
    for (int k = 0; k < SPECTRUM_N; k += 37) {
        double re = 0, im = 0;
        for (int n = 0; n < SPECTRUM_N; n++) {
            re += x[n] * cos(2 * PI * k * n / SPECTRUM_N);
            im -= x[n] * sin(2 * PI * k * n / SPECTRUM_N);
        }
        double e = fabs(re / SPECTRUM_N - sp.re[k]) + fabs(im / SPECTRUM_N - sp.im[k]);
        if (e > worst) worst = e;
    }
    CHECK(worst < 6);                                  /* a few LSBs of rounding */
}

int main(void)
{
    RUN(test_the_log_is_eighths_of_an_octave);
    RUN(test_silence_shows_nothing);
    RUN(test_a_tone_lights_its_own_bar);
    RUN(test_a_full_scale_tone_nearly_fills_the_height);
    RUN(test_40_dB_quieter_is_106_pixels_lower);
    RUN(test_bars_fall_and_peaks_hold_then_fall);
    RUN(test_the_fft_is_a_dft_divided_by_1024);
    CHECK_DONE();
}
