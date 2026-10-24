// Delta-sigma audio output, checked the way the board hears it: the 1-bit
// stream at 50 MHz goes through a model of the board's RC filter (3.3 kOhm,
// 4.7 nF, about 10 kHz), and the audio band (20 Hz - 20 kHz) of the result
// is analysed with an FFT.
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>
#include "Vdac_dsm.h"
#include "verilated.h"

static const double FS = 50e6, RC = 3300 * 4.7e-9;
static const int N = 1 << 20;               // 21 ms
static const int TONE_BIN = 21;             // 1001 Hz, a whole number of cycles
static const int BAND_LO = 1, BAND_HI = (int)(20000.0 * N / FS);

static void fft(std::vector<std::complex<double>> &a)
{
    int n = (int)a.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        std::complex<double> w(cos(2 * M_PI / len), -sin(2 * M_PI / len));
        for (int i = 0; i < n; i += len) {
            std::complex<double> wk(1);
            for (int k = 0; k < len / 2; k++, wk *= w) {
                auto u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

struct Result { double tone_amp, noise_db, max_bin_db; };

// Plays a 1 kHz tone of the given amplitude (full scale = 32767) and
// returns the tone's amplitude after the RC filter (full-scale square = 1)
// and the in-band noise and largest spur, in dB relative to full scale.
static Result run(double amplitude, int dc = 0)
{
    Vdac_dsm dut;
    double alpha = 1 - exp(-1 / (FS * RC)), y = 0;
    std::vector<std::complex<double>> s(N);
    dut.rst = 1;
    for (int i = 0; i < 4; i++) { dut.clk = 0; dut.eval(); dut.clk = 1; dut.eval(); }
    dut.rst = 0;
    for (int i = -N / 4; i < N; i++) {      // first quarter: let it settle
        int x = dc + (int)lround(amplitude * sin(2 * M_PI * TONE_BIN * (double)i / N));
        dut.in = (uint16_t)(int16_t)x;
        dut.clk = 0; dut.eval(); dut.clk = 1; dut.eval();
        y += ((dut.out ? 1.0 : -1.0) - y) * alpha;
        if (i >= 0) s[i] = y;
    }
    fft(s);
    Result r;
    r.tone_amp = 2 * std::abs(s[TONE_BIN]) / N;
    double noise = 0, max_bin = 0;
    for (int k = BAND_LO; k <= BAND_HI; k++) {
        if (amplitude != 0 && abs(k - TONE_BIN) <= 1) continue;
        double p = std::norm(2.0 * s[k] / (double)N) / 2;   // mean-square power of that bin
        noise += p;
        if (p > max_bin) max_bin = p;
    }
    // reference: a full-scale sine has power 1/2
    r.noise_db = 10 * log10(noise / 0.5 + 1e-30);
    r.max_bin_db = 10 * log10(max_bin / 0.5 + 1e-30);
    return r;
}

int main()
{
    int failures = 0;
    double rc_gain = 1 / sqrt(1 + pow(1001.4 * 2 * M_PI * RC, 2));
    const double scale = 13.0 / 16;   // the modulator's input scaling (keeps it stable)
    struct { double amp; double min_snr; } cases[] = {{32767, 90}, {16384, 90}, {100, 40}};
    for (auto &c : cases) {
        Result r = run(c.amp);
        double expected = c.amp / 32768 * scale * rc_gain;
        double tone_db = 20 * log10(r.tone_amp);
        double snr = tone_db - r.noise_db;  // both relative to a full-scale sine
        bool ok = fabs(r.tone_amp - expected) < expected * 0.005 && snr >= c.min_snr;
        printf("tone %5.0f: amplitude %.6f (expected %.6f), in-band SNR %.1f dB (need %.0f): %s\n",
               c.amp, r.tone_amp, expected, snr, c.min_snr, ok ? "ok" : "FAIL");
        failures += !ok;
    }
    // Near silence a delta-sigma loop can fall into patterns whose tones land
    // in the audio band. Silence and tiny DC offsets must stay clean.
    for (int dc : {0, 1, -1, 3, 17, -40}) {
        Result q = run(0, dc);
        bool ok = q.max_bin_db < -120;
        printf("silence + DC %3d: in-band noise %.1f dBFS, largest tone %.1f dBFS (need < -120): %s\n",
               dc, q.noise_db, q.max_bin_db, ok ? "ok" : "FAIL");
        failures += !ok;
    }
    return failures ? 1 : 0;
}
