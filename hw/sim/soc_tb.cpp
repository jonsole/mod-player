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
#include "sdram_model.h"

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
    static SdramModel sdram;
    std::string text;
    int bit_timer = -1, bit_index = 0, byte = 0;
    dut->rst = 1;
    dut->uart_rx = 1;
    for (long long cycle = 0; cycle < max_cycles; cycle++) {
        if (cycle == 10)
            dut->rst = 0;
        dut->clk = 0;
        dut->eval();
        sdram.step(dut->sd_cke, dut->sd_cs_n, dut->sd_ras_n, dut->sd_cas_n, dut->sd_we_n,
                   dut->sd_ba, dut->sd_a, dut->sd_dqm, dut->sd_dq_o, dut->sd_dq_oe);
        dut->sd_dq_i = sdram.dq_in;
        dut->clk = 1;
        dut->eval();
        if (sdram.failures) {
            printf("serial: %s\nFAIL: SDRAM rules broken\n", text.c_str());
            return 1;
        }
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
                    sdram.finish();
                    printf("serial: %s\n%s: found \"%s\" after %lld cycles (SDRAM: %lld reads, %lld writes, %lld refreshes)\n",
                           text.c_str(), sdram.failures ? "FAIL" : "ok", expect, cycle,
                           sdram.reads, sdram.writes, sdram.refreshes);
                    delete dut;
                    return sdram.failures ? 1 : 0;
                }
            }
        }
    }
    printf("serial: %s\nFAIL: \"%s\" not seen in %lld cycles\n", text.c_str(), expect, max_cycles);
    delete dut;
    return 1;
}
