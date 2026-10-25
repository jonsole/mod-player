// TMDS serialiser test: random characters on the pixel clock, the 5x clock
// running five times as fast and rising with it. The bit stream (d0 then d1
// each 5x clock) must carry every character, bit 0 first, in order, with
// the clock lane's 0000011111 marking character boundaries.
//
//   serialiser_tb
#include <cstdio>
#include <deque>
#include "Vtmds_serialiser.h"
#include "verilated.h"

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    Vtmds_serialiser *dut = new Vtmds_serialiser;
    uint32_t rng = 3;
    auto rnd = [&rng]() { rng = rng * 1103515245u + 12345u; return rng >> 8; };
    std::deque<uint32_t> sent[3];
    std::deque<int> bits[4];
    int failures = 0, checked = 0;
    for (int px = 0; px < 20000; px++) {
        uint32_t c[3] = {rnd() & 0x3FF, rnd() & 0x3FF, rnd() & 0x3FF};
        dut->tmds0 = c[0]; dut->tmds1 = c[1]; dut->tmds2 = c[2];
        for (int k = 0; k < 3; k++) sent[k].push_back(c[k]);
        for (int f = 0; f < 5; f++) {
            dut->clk_x5 = 1;
            dut->clk_pixel = f == 0;
            dut->eval();
            for (int l = 0; l < 4; l++) {
                bits[l].push_back(dut->d0 >> l & 1);
                bits[l].push_back(dut->d1 >> l & 1);
            }
            dut->clk_x5 = 0; dut->clk_pixel = 0;
            dut->eval();
        }
    }
    // find the clock lane's character boundary: five 1s after five 0s
    size_t start = 0;
    while (start + 10 <= bits[3].size()) {
        int v = 0;
        for (int b = 0; b < 10; b++) v |= bits[3][start + b] << b;
        if (v == 0x01F && start > 20) break;      // the 10-bit value 0000011111
        start++;
    }
    // the stream from `start` must be a run of the characters sent, in order
    size_t first = 0;
    int v0 = 0;
    for (int b = 0; b < 10; b++) v0 |= bits[0][start + b] << b;
    while (first < sent[0].size() && sent[0][first] != (uint32_t)v0) first++;
    for (size_t pos = start, n = first; pos + 10 <= bits[0].size() && n < sent[0].size(); pos += 10, n++) {
        int clk = 0;
        for (int b = 0; b < 10; b++) clk |= bits[3][pos + b] << b;
        if (clk != 0x01F && failures++ < 5) printf("FAIL: clock lane %03x at bit %zu\n", clk, pos);
        for (int k = 0; k < 3; k++) {
            int v = 0;
            for (int b = 0; b < 10; b++) v |= bits[k][pos + b] << b;
            if ((uint32_t)v != sent[k][n] && failures++ < 5)
                printf("FAIL: lane %d character %zu: sent %03x, serialised %03x\n", k, n, sent[k][n], v);
        }
        checked++;
    }
    if (checked < 19000) { printf("FAIL: only %d characters lined up\n", checked); failures++; }
    printf("tmds_serialiser: %s, %d characters\n", failures ? "FAILED" : "ok", checked);
    delete dut;
    return failures ? 1 : 0;
}
