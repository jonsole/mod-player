/* The HDMI Resampler's C model (host/resampler.c): Paula frames in at the
 * colour-clock rate, 48 kHz out. These tests pin its rate and its sound;
 * the Verilog must then match the model exactly (hw/sim/resampler_tb.cpp). */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "resampler.h"

#define CCK_HZ 3546895.0
#define OUT_HZ 48000.0
#define PI 3.14159265358979323846

static int32_t out_l[60000], out_r[60000];

/* Feeds n frames of a sine (left) and its negation (right); returns outputs. */
static int run_sine(double hz, double amp, long n)
{
    resampler_t rs;
    resampler_reset(&rs);
    int count = 0;
    for (long i = 0; i < n; i++) {
        int32_t x = (int32_t)lround(amp * sin(2 * PI * hz * (double)i / CCK_HZ));
        int32_t y[2];
        if (resampler_push(&rs, x, -x, y)) {
            if (count < 60000) {
                out_l[count] = y[0];
                out_r[count] = y[1];
            }
            count++;
        }
    }
    return count;
}

/* Amplitude of the output at hz, after skipping the filter's start-up. */
static double tone_amplitude(const int32_t *y, int count, double hz)
{
    double re = 0, im = 0;
    int skip = 200, n = count - skip;
    for (int k = skip; k < count; k++) {
        double ph = 2 * PI * hz * (double)k / OUT_HZ;
        re += y[k] * cos(ph);
        im += y[k] * sin(ph);
    }
    return 2 * sqrt(re * re + im * im) / n;
}

static double rms(const int32_t *y, int count)
{
    double s = 0;
    int skip = 200;
    for (int k = skip; k < count; k++) s += (double)y[k] * y[k];
    return sqrt(s / (count - skip));
}

/* Output scale: Paula frame x comes out as x * 512 in 24 bits, the same
 * full scale as the audio jack's x * 2 in 16 bits. */
#define GAIN 512.0

static void test_output_rate_is_exactly_48k(void)
{
    resampler_t rs;
    resampler_reset(&rs);
    long outputs = 0;
    long frames = 3546895L * 3;           /* three seconds */
    for (long i = 0; i < frames; i++) {
        int32_t y[2];
        outputs += resampler_push(&rs, 0, 0, y);
    }
    CHECK_EQ(outputs, 48000L * 3);
}

static void test_silence_in_silence_out(void)
{
    resampler_t rs;
    resampler_reset(&rs);
    int bad = 0;
    for (long i = 0; i < 100000; i++) {
        int32_t y[2];
        if (resampler_push(&rs, 0, 0, y) && (y[0] != 0 || y[1] != 0)) bad++;
    }
    CHECK_EQ(bad, 0);
}

static void test_dc_passes_at_unity(void)
{
    resampler_t rs;
    resampler_reset(&rs);
    int count = 0, worst = 0;
    for (long i = 0; i < 200000; i++) {
        int32_t y[2];
        if (resampler_push(&rs, 1000, -16384, y) && ++count > 100) {
            int e0 = abs(y[0] - 1000 * 512), e1 = abs(y[1] - (-16384 * 512));
            if (e0 > worst) worst = e0;
            if (e1 > worst) worst = e1;
        }
    }
    /* every phase of the table sums to exactly 2^18 */
    CHECK_EQ(worst, 0);
}

static void test_full_scale_saturates(void)
{
    resampler_t rs;
    resampler_reset(&rs);
    int32_t lo = 0, hi = 0;
    for (long i = 0; i < 400000; i++) {
        /* full-scale square wave at 1 kHz: overshoot must clip, not wrap */
        int32_t x = ((i / 1773) & 1) ? 32767 : -32768;
        int32_t y[2];
        if (resampler_push(&rs, x, x, y)) {
            if (y[0] < lo) lo = y[0];
            if (y[0] > hi) hi = y[0];
        }
    }
    CHECK_EQ(hi, 8388607);
    CHECK_EQ(lo, -8388608);
}

static void test_passband_is_flat(void)
{
    static const double hz[] = {100, 1000, 5000, 10000, 15000, 20000};
    for (unsigned t = 0; t < sizeof hz / sizeof hz[0]; t++) {
        int count = run_sine(hz[t], 10000, 3546895L / 4);
        double gain_l = tone_amplitude(out_l, count, hz[t]) / (10000 * GAIN);
        double gain_r = tone_amplitude(out_r, count, hz[t]) / (10000 * GAIN);
        double db = 20 * log10(gain_l);
        /* the CIC stage droops 0.47 dB at 20 kHz; the FIR is flat */
        if (!(db > -0.6 && db < 0.05) || fabs(gain_l - gain_r) > 1e-4) {
            printf("  %g Hz: %.3f dB (L), %.3f dB (R)\n", hz[t], db, 20 * log10(gain_r));
            CHECK(0);
        }
    }
}

static void test_aliases_are_rejected(void)
{
    /* Tones that would fold into 0..20 kHz at 48 kHz: what comes out,
     * at any frequency, must be at least 70 dB down. */
    static const double hz[] = {28500, 30000, 40000, 47000, 50000, 70000, 100000,
                                201000, 443000, 1000000};
    for (unsigned t = 0; t < sizeof hz / sizeof hz[0]; t++) {
        int count = run_sine(hz[t], 16000, 3546895L / 4);
        double db = 20 * log10(rms(out_l, count) * sqrt(2) / (16000 * GAIN) + 1e-12);
        if (db > -70) {
            printf("  %g Hz leaks through at %.1f dB\n", hz[t], db);
            CHECK(0);
        }
    }
}

static void test_low_level_tone_is_clean(void)
{
    /* A quiet 1 kHz tone: everything that isn't the tone (rounding and
     * phase steps) must be at least 90 dB below full scale. */
    int count = run_sine(1000, 100, 3546895L / 4);
    double amp = tone_amplitude(out_l, count, 1000);
    double re = 0, im = 0, err = 0;
    int skip = 200;
    for (int k = skip; k < count; k++) {
        double ph = 2 * PI * 1000 * (double)k / OUT_HZ;
        re += out_l[k] * cos(ph);
        im += out_l[k] * sin(ph);
    }
    double phase = atan2(re, im);
    for (int k = skip; k < count; k++) {
        double e = out_l[k] - amp * sin(2 * PI * 1000 * (double)k / OUT_HZ + phase);
        err += e * e;
    }
    err = sqrt(err / (count - skip));
    double db = 20 * log10(err / 8388608.0);
    if (db > -90) {
        printf("  residual %.1f dBFS\n", db);
        CHECK(0);
    }
}

int main(void)
{
    RUN(test_output_rate_is_exactly_48k);
    RUN(test_silence_in_silence_out);
    RUN(test_dc_passes_at_unity);
    RUN(test_full_scale_saturates);
    RUN(test_passband_is_flat);
    RUN(test_aliases_are_rejected);
    RUN(test_low_level_tone_is_clean);
    CHECK_DONE();
}
