// The text console's bus side on its own (ADR-0012): writes as fast as the
// bus allows (some in back-to-back clocks, so most find the previous write
// still crossing to the pixel clock), with random gaps and byte enables.
// Every write the bus accepts must reach the text RAM exactly once, in
// order, with its data; status reads must say a console is there and count
// frames.
//
// Clocks run at their exact ratio (50 MHz : 25.2 MHz = 125 : 63); the
// testbench plays hdmi_tx's part by stepping pixel_x / pixel_y.
#include <cstdio>
#include <cstdlib>
#include <deque>
#include "Vtext_console.h"
#include "verilated.h"

struct Write { uint16_t cell, data; uint8_t be; };

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int writes = argc > 1 ? atoi(argv[1]) : 20000;
    Vtext_console *dut = new Vtext_console;
    std::deque<Write> in_flight;
    uint32_t rng = 99;
    auto rnd = [&rng]() { rng = rng * 1103515245u + 12345u; return rng >> 8; };
    int failures = 0, accepted = 0, landed = 0, waited = 0, gap = 0;
    uint32_t x = 0, y = 0, frames_seen = 0, last_status = 0;
    bool reading = false;

    dut->rst = 1; dut->rst_pixel = 1;
    uint64_t t_sys = 0, t_pix = 0, cycles = 0;
    while (landed < writes && cycles < 50000000ULL) {
        if (t_sys <= t_pix) {
            if (cycles == 10) { dut->rst = 0; dut->rst_pixel = 0; }
            // offer the next access, as PicoRV32 holds one until ready
            if (!dut->bus_valid && cycles > 20 && accepted < writes) {
                if (gap > 0) gap--;
                else {
                    reading = rnd() % 16 == 0;
                    dut->bus_valid = 1;
                    dut->bus_addr = reading ? 0x1000 : rnd() % 1060;
                    dut->bus_wdata = (uint16_t)rnd();
                    dut->bus_wstrb = reading ? 0 : (rnd() % 3 + 1);
                }
            }
            dut->clk = 0; dut->eval();
            bool take = dut->bus_valid && dut->bus_ready;
            if (dut->bus_valid && !dut->bus_ready) waited++;
            if (take && reading) {
                uint32_t s = dut->bus_rdata;
                if (!(s >> 31)) { if (failures++ < 5) printf("FAIL: status %08x doesn't say a console is there\n", s); }
                if ((s & 0xFFFFFF) < (last_status & 0xFFFFFF)) { if (failures++ < 5) printf("FAIL: frame count went back\n"); }
                last_status = s;
            } else if (take) {
                in_flight.push_back({(uint16_t)dut->bus_addr, (uint16_t)dut->bus_wdata, (uint8_t)(dut->bus_wstrb & 3)});
                accepted++;
            }
            dut->clk = 1; dut->eval();
            if (take) {
                dut->bus_valid = 0;
                gap = rnd() % 4 == 0 ? (int)(rnd() % 12) : 0;   // mostly back to back
            }
            t_sys += 63;
            cycles++;
        } else {
            dut->pixel_x = x; dut->pixel_y = y;
            dut->clk_pixel = 1; dut->eval();
            if (dut->dbg_we) {
                if (in_flight.empty()) {
                    if (failures++ < 5) printf("FAIL: the text RAM took a write nobody made\n");
                } else {
                    Write w = in_flight.front();
                    in_flight.pop_front();
                    if (w.cell != dut->dbg_cell || w.data != dut->dbg_data || w.be != dut->dbg_be)
                        if (failures++ < 5)
                            printf("FAIL: write %d: RAM got cell %d data %04x be %d, bus took cell %d data %04x be %d\n",
                                   landed, dut->dbg_cell, dut->dbg_data, dut->dbg_be, w.cell, w.data, w.be);
                    landed++;
                }
            }
            dut->clk_pixel = 0; dut->eval();
            if (++x == 800) { x = 0; if (++y == 525) { y = 0; frames_seen++; } }
            t_pix += 125;
        }
    }
    if (landed != writes) { failures++; printf("FAIL: %d of %d writes reached the text RAM\n", landed, writes); }
    if (waited == 0) { failures++; printf("FAIL: no write ever had to wait: the test didn't exercise the handover\n"); }
    if (frames_seen < 2) { failures++; printf("FAIL: only %u frames: too short to check the frame count\n", frames_seen); }
    if ((last_status & 0xFFFFFF) + 1 < frames_seen) {
        failures++;
        printf("FAIL: status counts %u frames, the testbench showed %u\n", last_status & 0xFFFFFF, frames_seen);
    }
    printf("text_console bus: %s, %d writes landed in order, %d clocks spent waiting, %u frames\n",
           failures ? "FAILED" : "ok", landed, waited, frames_seen);
    delete dut;
    return failures ? 1 : 0;
}
