// Verilog Resampler vs its C model (ADR-0005, ADR-0010).
//
// Both get the same Paula frames; every 48 kHz output the Verilog produces
// must equal the model's, in order, and none may be skipped (overrun).
// Frames arrive 3..20 clocks apart (random), from 3 (the fastest the
// design allows) to slower than real time (14.1).
//
//   resampler_tb --noise seed seconds    random full-range frames, in runs
//   resampler_tb --square seconds        full-scale square waves (saturation)
//   resampler_tb song.mod seconds options  a Song through the Replayer and
//                                           the Paula C model (options 0..7)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include "Vresampler.h"
#include "verilated.h"

#include "mod.h"
#include "paula_model.h"
#include "replayer.h"
#include "resampler.h"

static Vresampler *dut;
static resampler_t model;
static std::deque<std::pair<int32_t, int32_t>> expected;
static uint64_t outputs;
static uint32_t rng = 1;
static int failures;

static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void clock_once()
{
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    dut->in_valid = 0;
    if (dut->out_valid) {
        int32_t l = ((int32_t)(dut->out_l << 8)) >> 8, r = ((int32_t)(dut->out_r << 8)) >> 8;
        if (expected.empty()) {
            if (failures++ < 5) printf("FAIL: output %llu from Verilog only\n", (unsigned long long)outputs);
        } else {
            auto e = expected.front();
            expected.pop_front();
            if (l != e.first || r != e.second) {
                if (failures++ < 5)
                    printf("FAIL: output %llu: Verilog (%d, %d), model (%d, %d)\n",
                           (unsigned long long)outputs, l, r, e.first, e.second);
            }
        }
        outputs++;
    }
}

static void frame(int32_t l, int32_t r)
{
    int32_t y[2];
    if (resampler_push(&model, l, r, y)) expected.push_back({y[0], y[1]});
    dut->in_valid = 1;
    dut->in_l = (uint16_t)l;
    dut->in_r = (uint16_t)r;
    clock_once();
    for (uint32_t gap = 2 + rnd() % 18; gap > 0; gap--) clock_once();
}

static int finish(const char *what)
{
    for (int i = 0; i < 400; i++) clock_once();   // let the last output finish
    if (!expected.empty() && failures++ < 5)
        printf("FAIL: %zu outputs from the model only\n", expected.size());
    if (dut->overrun) {
        printf("FAIL: overrun (an output was skipped)\n");
        failures++;
    }
    printf("%s: %s, %llu outputs\n", what, failures ? "FAILED" : "ok", (unsigned long long)outputs);
    return failures ? 1 : 0;
}

#define NULL_SAMPLE_BYTES (65536u * 2)

static int run_song(const char *path, double seconds, uint32_t options)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot read %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t null_addr = (size + 1) & ~1u;
    std::vector<uint8_t> mem(null_addr + NULL_SAMPLE_BYTES);
    if (fread(mem.data(), 1, size, f) != size) { fclose(f); return 1; }
    fclose(f);
    static mod_song_t song;
    if (mod_load(&song, mem.data(), size) != MOD_OK) { printf("bad MOD file %s\n", path); return 1; }
    static paula_model_t paula;
    static replayer_t rp;
    paula_model_reset(&paula, mem.data(), (uint32_t)mem.size());
    paula_model_write(&paula, PAULA_OPTIONS, options);
    replayer_start(&rp, &song, mem.data(), 0, null_addr, paula_model_port(&paula));
    uint64_t frames = (uint64_t)(seconds * 3546895);
    uint32_t tick_left = 0;
    for (uint64_t i = 0; i < frames; i++) {
        if (tick_left == 0) tick_left = rp.stopped ? 70940 : replayer_tick(&rp);
        tick_left--;
        paula_frame_t fr = paula_model_step(&paula);
        frame(fr.left, fr.right);
    }
    char what[512];
    snprintf(what, sizeof what, "%s %gs options %u", path, seconds, options);
    return finish(what);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    dut = new Vresampler;
    resampler_reset(&model);
    dut->rst = 1;
    for (int i = 0; i < 4; i++) clock_once();
    dut->rst = 0;

    if (argc == 4 && strcmp(argv[1], "--noise") == 0) {
        rng = (uint32_t)atoi(argv[2]);
        uint64_t frames = (uint64_t)(atof(argv[3]) * 3546895);
        int32_t l = 0, r = 0;
        for (uint64_t i = 0; i < frames; i++) {
            if (rnd() % 64 == 0) {    // hold values for a while, like Paula's samples
                l = (int32_t)(rnd() % 65536) - 32768;
                r = (int32_t)(rnd() % 65536) - 32768;
            }
            frame(l, r);
        }
        char what[64];
        snprintf(what, sizeof what, "noise seed %s", argv[2]);
        return finish(what);
    }
    if (argc == 3 && strcmp(argv[1], "--square") == 0) {
        uint64_t frames = (uint64_t)(atof(argv[2]) * 3546895);
        for (uint64_t i = 0; i < frames; i++) {
            int32_t l = ((i / 1773) & 1) ? 32767 : -32768;      // 1 kHz
            int32_t r = ((i / 113) & 1) ? -32768 : 32767;       // 15.7 kHz
            frame(l, r);
        }
        return finish("full-scale squares");
    }
    if (argc == 4)
        return run_song(argv[1], atof(argv[2]), (uint32_t)atoi(argv[3]));
    fprintf(stderr, "usage: resampler_tb --noise seed seconds | --square seconds | song.mod seconds options\n");
    return 2;
}
