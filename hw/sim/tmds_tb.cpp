// TMDS video encoder test: random bytes in runs of video and control,
// every character decodes back to its byte and the running disparity
// (ones minus zeros sent since the last control period) stays within the
// DVI bound.
//
//   tmds_tb seed characters
#include <cstdio>
#include <cstdlib>
#include "Vtmds_encoder.h"
#include "verilated.h"

static uint32_t rng;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static int decode(uint16_t q)
{
    uint16_t d = (q & 0x200) ? (q ^ 0xFF) : q;
    int out = d & 1;
    for (int i = 1; i < 8; i++) {
        int b = ((d >> i) ^ (d >> (i - 1))) & 1;
        if (!(d & 0x100)) b ^= 1;
        out |= b << i;
    }
    return out;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    rng = argc > 1 ? (uint32_t)atoi(argv[1]) : 1;
    long n = argc > 2 ? atol(argv[2]) : 1000000;
    Vtmds_encoder *dut = new Vtmds_encoder;
    int failures = 0, video = 0, run = 0, disparity = 0, worst = 0, want = -1;
    for (long i = 0; i < n; i++) {
        if (run-- <= 0) { video = rnd() % 8 != 0; run = 1 + rnd() % (video ? 700 : 20); }
        int d = (rnd() % 4 == 0) ? (rnd() % 2 ? 0x00 : 0xFF) : rnd() & 0xFF;   // extremes often
        dut->video = video;
        dut->d = d;
        dut->other = 0x354;
        dut->clk = 0; dut->eval();
        dut->clk = 1; dut->eval();
        // q now holds the character for this clock's input
        want = video ? d : -1;
        if (want >= 0) {
            uint16_t q = dut->q;
            int ones = 0;
            for (int b = 0; b < 10; b++) ones += (q >> b) & 1;
            disparity += 2 * ones - 10;
            if (disparity > worst) worst = disparity;
            if (-disparity > worst) worst = -disparity;
            if (decode(q) != want && failures++ < 5) printf("FAIL: %02x encoded as %03x decodes to %02x\n", want, q, decode(q));
            if ((disparity > 18 || disparity < -18) && failures++ < 5) printf("FAIL: disparity %d\n", disparity);
        } else {
            disparity = 0;
        }
    }
    printf("tmds_encoder: %s, %ld characters, worst disparity %d\n", failures ? "FAILED" : "ok", n, worst);
    delete dut;
    return failures ? 1 : 0;
}
