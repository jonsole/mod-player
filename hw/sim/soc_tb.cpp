// Runs the SoC and decodes its serial output (8N1), passing when the
// expected text appears.
//
//   soc_tb "expected text" [max_cycles]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "Vsoc.h"
#include "verilated.h"

static const int CLK_HZ = 50000000, BAUD = 1000000, BIT = CLK_HZ / BAUD;

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 2) {
        printf("usage: soc_tb \"expected text\" [max_cycles]\n");
        return 2;
    }
    const char *expect = argv[1];
    long long max_cycles = argc > 2 ? atoll(argv[2]) : 20000000LL;
    Vsoc *dut = new Vsoc;
    std::string text;
    int bit_timer = -1, bit_index = 0, byte = 0;
    dut->rst = 1;
    dut->uart_rx = 1;
    for (long long cycle = 0; cycle < max_cycles; cycle++) {
        if (cycle == 10)
            dut->rst = 0;
        dut->clk = 0;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        // 8N1 receiver: sample each bit in its middle
        if (bit_timer < 0) {
            if (!dut->uart_tx) {
                bit_timer = BIT + BIT / 2;
                bit_index = 0;
                byte = 0;
            }
        } else if (--bit_timer == 0) {
            if (bit_index < 8) {
                byte |= (dut->uart_tx & 1) << bit_index++;
                bit_timer = BIT;
            } else {
                if (!dut->uart_tx)
                    printf("FAIL: framing error after byte 0x%02x\n", byte);
                text += (char)byte;
                bit_timer = -1;
                if (text.find(expect) != std::string::npos) {
                    printf("serial: %s\nok: found \"%s\" after %lld cycles\n", text.c_str(), expect, cycle);
                    delete dut;
                    return 0;
                }
            }
        }
    }
    printf("serial: %s\nFAIL: \"%s\" not seen in %lld cycles\n", text.c_str(), expect, max_cycles);
    delete dut;
    return 1;
}
