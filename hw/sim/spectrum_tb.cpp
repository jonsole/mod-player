// The spectrum analyser against its C model (issue #24, ADR-0014): 48 kHz
// samples go into both; whenever the hardware starts an analysis the model
// analyses the same 1024 samples, and when the hardware has updated its
// bars every bar and peak must be the model's. Every pixel the analyser
// draws is checked too, against the bars shown in that frame.
//
//   spectrum_tb frames [song.mod]
// Without a Song the input is a programme of tones, sweeps, noise, clipping
// and silence; with one it is the Song, through the Paula and Resampler C
// models.
//
// Clocks run at their exact ratio (50 MHz : 25.2 MHz = 125 : 63); the
// testbench plays hdmi_tx's part by stepping pixel_x / pixel_y.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#include "Vspectrum.h"
#include "verilated.h"
#include "spectrum.h"
#include "mod.h"
#include "paula_model.h"
#include "replayer.h"
#include "resampler.h"

static int failures;
#define FAIL(...) do { if (failures++ < 20) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static spectrum_t model;

// The analyser's pixel at (x, y) from the bars shown: 0 if none.
static uint32_t pixel(int x, int y, const uint8_t *h, const uint8_t *p)
{
    if (x < 8 || x >= 632 || y < 288 || y >= 480) return 0;
    int bx = x - 8, bar = bx / 13, sub = bx % 13, pos = 479 - y;
    if (sub >= 12) return 0;
    if (p[bar] && (pos == p[bar] || pos == p[bar] + 1)) return 0xE0E0E0;
    if (pos < h[bar]) {
        int red = pos < 96 ? pos * 5 / 2 : 255, green = pos < 96 ? 255 : 255 - (pos - 96) * 5 / 2;
        return (uint32_t)(red << 16 | green << 8 | 0x20);
    }
    return 0;
}

// The test programme: what the samples are in each stretch of time.
static int32_t programme(long n)
{
    double t = n / 48000.0;
    long part = n / 24000;                                    // half a second each
    static uint32_t rng = 1;
    switch (part % 8) {
    case 0: return 0;                                         // silence
    case 1: return (int32_t)(20000 * sin(2 * M_PI * 440 * t)) * 128;
    case 2: {                                                 // a sweep 50 Hz .. 20 kHz
        double f = 50 * pow(400.0, (n % 24000) / 24000.0);
        static double ph;
        ph += 2 * M_PI * f / 48000.0;
        return (int32_t)(30000 * sin(ph)) * 128;
    }
    case 3: rng = rng * 1103515245u + 12345u; return (int32_t)((rng >> 8) % 60001 - 30000) * 128;   // noise
    case 4: return (int32_t)(65535 * sin(2 * M_PI * 3000 * t)) * 128;            // full scale
    case 5: return (int32_t)(300 * sin(2 * M_PI * 120 * t) + 3000 * sin(2 * M_PI * 9000 * t)) * 128;
    case 6: return ((n / 40) & 1) ? 8000000 : -8000000;      // a square wave, clipped hard
    default: return 0;                                        // silence again: bars fall
    }
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int frames = argc > 1 ? atoi(argv[1]) : 120;
    const char *song_path = argc > 2 ? argv[2] : nullptr;

    // the Song, through the Paula and Resampler models
    static std::vector<uint8_t> mem;
    static mod_song_t song;
    static paula_model_t paula;
    static replayer_t rp;
    static resampler_t rs;
    std::deque<std::pair<int32_t, int32_t>> song_samples;
    uint32_t tick_left = 0;
    if (song_path) {
        FILE *f = fopen(song_path, "rb");
        if (!f) { printf("cannot read %s\n", song_path); return 2; }
        fseek(f, 0, SEEK_END);
        uint32_t size = (uint32_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        uint32_t null_addr = (size + 1) & ~1u;
        mem.assign(null_addr + 131072, 0);
        if (fread(mem.data(), 1, size, f) != size) return 2;
        fclose(f);
        if (mod_load(&song, mem.data(), size) != MOD_OK) { printf("bad MOD file\n"); return 2; }
        paula_model_reset(&paula, mem.data(), (uint32_t)mem.size());
        replayer_start(&rp, &song, mem.data(), 0, null_addr, paula_model_port(&paula));
        resampler_reset(&rs);
    }

    spectrum_reset(&model);
    Vspectrum *dut = new Vspectrum;
    dut->rst = 1; dut->rst_pixel = 1; dut->enable = 1;
    uint64_t t_sys = 0, t_pix = 0, cycles = 0, phase = 0;
    long sample_no = 0;
    int analyses = 0, frames_seen = 0, pixels_lit = 0, x = 0, y = 0, max_bar = 0;
    bool pending = false;
    uint8_t shown_h[48] = {0}, shown_p[48] = {0};
    std::deque<std::pair<int, int>> named;                    // pixel_x/y, latest last

    while (frames_seen < frames) {
        if (t_sys <= t_pix) {
            if (cycles == 10) { dut->rst = 0; dut->rst_pixel = 0; }
            // the next sample, to the hardware and the model
            auto send_sample = [&]() {
                int32_t l, r;
                if (song_path) {
                    while (song_samples.empty()) {
                        if (tick_left == 0) tick_left = replayer_tick(&rp);
                        tick_left--;
                        paula_frame_t fr = paula_model_step(&paula);
                        int32_t yv[2];
                        if (resampler_push(&rs, fr.left, fr.right, yv)) song_samples.push_back({yv[0], yv[1]});
                    }
                    l = song_samples.front().first;
                    r = song_samples.front().second;
                    song_samples.pop_front();
                } else {
                    l = r = programme(sample_no) / 2;
                }
                sample_no++;
                dut->in_valid = 1;
                dut->in_l = (uint32_t)l & 0xFFFFFF;
                dut->in_r = (uint32_t)r & 0xFFFFFF;
                spectrum_push(&model, l, r);
            };
            dut->in_valid = 0;
            if ((phase += 48000) >= 50000000) {                // 48 kHz
                phase -= 50000000;
                send_sample();
            }
            dut->clk = 0; dut->eval();
            if (dut->dbg_start && !dut->in_valid && analyses % 2 == 0) {
                // every other analysis, a sample arrives in the very clock it
                // starts: it is the newest of the 1024, not the oldest
                send_sample();
                dut->eval();
            }
            if (dut->dbg_start) {                              // the same samples as the hardware
                if (pending) FAIL("an analysis started before the last finished");
                spectrum_frame(&model);
                pending = true;
                analyses++;
            }
            dut->clk = 1; dut->eval();
            if (dut->dbg_done) {
                if (!pending) FAIL("bars updated without an analysis");
                pending = false;
                for (int b = 0; b < 48; b++) {
                    int hb = (int)(dut->dbg_bars[b / 4] >> (8 * (b % 4)) & 0xFF);
                    int hp = (int)(dut->dbg_peaks[b / 4] >> (8 * (b % 4)) & 0xFF);
                    if (hb != model.bar[b] || hp != model.peak[b])
                        FAIL("analysis %d bar %d: hardware %d (peak %d), model %d (peak %d; level %d)",
                             analyses, b, hb, hp, model.bar[b], model.peak[b], model.level[b]);
                    if (hb > max_bar) max_bar = hb;
                }
            }
            t_sys += 63;
            cycles++;
        } else {
            dut->pixel_x = x; dut->pixel_y = y;
            dut->clk_pixel = 1; dut->eval();
            if (dut->dbg_copy || (x == 0 && y == 480)) {      // the display's copy, at this edge
                for (int b = 0; b < 48; b++) {
                    shown_h[b] = (uint8_t)(dut->dbg_bars[b / 4] >> (8 * (b % 4)));
                    shown_p[b] = (uint8_t)(dut->dbg_peaks[b / 4] >> (8 * (b % 4)));
                }
            }
            dut->clk_pixel = 0; dut->eval();
            // the colour now on rgb is for the pixel named 5 clocks ago
            named.push_back({x, y});
            if (named.size() > 5) named.pop_front();
            if (named.size() == 5) {
                uint32_t want = pixel(named.front().first, named.front().second, shown_h, shown_p);
                bool got_on = dut->on;
                if (got_on != (want != 0) || (want && dut->rgb != want))
                    FAIL("pixel (%d, %d): on %d rgb %06x, want %06x", named.front().first,
                         named.front().second, got_on, dut->rgb, want);
                if (want) pixels_lit++;
            }
            if (++x == 800) { x = 0; if (++y == 525) { y = 0; frames_seen++; } }
            t_pix += 125;
        }
    }
    if (analyses < frames - 3) FAIL("only %d analyses in %d frames", analyses, frames);
    if (max_bar == 0) FAIL("no bar ever rose");
    printf("spectrum: %s, %d analyses match the model, tallest bar %d, %d pixels drawn, %ld samples\n",
           failures ? "FAILED" : "ok", analyses, max_bar, pixels_lit, sample_no);
    delete dut;
    return failures ? 1 : 0;
}
