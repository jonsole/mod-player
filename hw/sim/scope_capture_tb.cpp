// One Voice's scope capture against its reference (hw/sim/scope_ref.h), on
// random input: values of every size and sign, held for any time (including
// exactly the stall limits and the 64-frame minimum), changes that come
// too soon (less than 64 frames after the last), Voices that go quiet, and frames arriving as close together as
// Paula's (4 clocks) or far apart. Every point the hardware writes must be
// the reference's next, and every capture must finish with all 316 points.
//
//   scope_capture_tb captures [seed]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "Vscope_one.h"
#include "verilated.h"
#include "scope_ref.h"

static uint32_t rng = 1;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

// the Voice's next value: mostly small steps, sometimes anything, the
// extremes, silence, or no change
static int next_value(int cur)
{
    switch (rnd() % 8) {
    case 0: return (int)(rnd() % 16384) - 8192;
    case 1: return rnd() % 2 ? 8128 : -8192;
    case 2: return 0;
    case 3: return cur;
    default: return std::max(-8192, std::min(8128, cur + (int)(rnd() % 801) - 400));
    }
}

// how long to hold it, in frames: often around a limit
static int next_hold()
{
    switch (rnd() % 6) {
    case 0: return 0;
    case 1: return 60 + (int)(rnd() % 6);            // around the 64-frame minimum
    case 2: return 125 + (int)(rnd() % 4);           // another round number to hit exactly
    case 3: return 1097 + (int)(rnd() % 4);          // around the held stall (1100)
    default: return (int)(rnd() % 400);
    }
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int want_captures = argc > 1 ? atoi(argv[1]) : 40;
    if (argc > 2) rng = (uint32_t)atoi(argv[2]);
    Vscope_one *dut = new Vscope_one;
    ScopeRef ref;
    int failures = 0, captures = 0;
    uint64_t checked = 0, cycles = 0;
    int cur = 0, hold = 0, gap_clocks = 4, until_frame = 0;
    bool started = false;

    dut->rst = 1; dut->audio_valid = 0; dut->start = 0; dut->clear = 0; dut->v = 0;
    for (int i = 0; i < 4; i++) { dut->clk = 0; dut->eval(); dut->clk = 1; dut->eval(); }
    dut->rst = 0;

    while (captures < want_captures && cycles < 4000000000ULL) {
        cycles++;
        dut->start = 0; dut->clear = 0; dut->audio_valid = 0;
        bool frame = false;
        int prev = cur;
        if (!started) {
            if (cycles % 50 == 0) {                         // a capture begins
                dut->start = 1;
                started = true;
                ref = ScopeRef();
                ref.phase = 1;
            }
        } else if (dut->phase == 3 && ref.phase == 3 && ref.expect.empty()) {
            dut->clear = 1;                                 // finished: back to idle
            started = false;
            captures++;
        } else if (--until_frame <= 0) {                    // a frame
            frame = true;
            until_frame = gap_clocks;
            if (rnd() % 64 == 0) gap_clocks = 4 + (int)(rnd() % 40);   // frames closer or further apart
            if (hold > 0) {
                hold--;
            } else {
                cur = next_value(cur);
                hold = next_hold();
                if (rnd() % 9 == 0) { cur = 0; hold = 55 + (int)(rnd() % 20); }
            }
            dut->audio_valid = 1;
            dut->v = (int16_t)cur;
        }
        dut->clk = 0; dut->eval();
        dut->clk = 1; dut->eval();
        if (frame && ref.phase != 0)
            scope_frame(ref, cur, prev);                    // the frame the edge just took
        if (dut->wen) {
            int index = dut->widx, point = (int8_t)(dut->wdat << 1) >> 1;
            checked++;
            if (ref.expect.empty()) {
                if (failures++ < 10) printf("FAIL: wrote point %d (%d) before the reference made it\n", index, point);
            } else {
                std::pair<int, int> want = ref.expect.front();
                ref.expect.pop_front();
                if ((want.first != index || want.second != point) && failures++ < 10)
                    printf("FAIL: capture %d wrote point %d = %d; the reference's next is %d = %d\n",
                           captures, index, point, want.first, want.second);
            }
        }
    }
    if (captures < want_captures) {
        failures++;
        printf("FAIL: only %d of %d captures finished\n", captures, want_captures);
    }
    printf("scope_capture: %s, %d captures, %llu points checked\n", failures ? "FAILED" : "ok", captures,
           (unsigned long long)checked);
    delete dut;
    return failures ? 1 : 0;
}
