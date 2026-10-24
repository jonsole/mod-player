// The board top level (top.v) in simulation: the SDRAM model sits on the
// SDRAM pins, serial goes through the FTDI pins, and the audio pins are
// passed through a model of the board's RC filters.
//
// Boots, starts a Song already placed in SDRAM whose sound is all on
// channel 1 (left), then checks that the left pin carries sound and the
// right pin silence: this catches swapped or dead audio pins, a wrong
// SDRAM clock phase, or a broken data bus.
//
//   board_tb song.mod
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Vboard_wrap.h"
#include "verilated.h"
#include "sdram_model.h"

static const int BIT = 50;   // 1 Mbaud at 50 MHz

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 2) { printf("usage: board_tb song.mod\n"); return 2; }
    std::vector<uint8_t> song;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot read %s\n", argv[1]); return 2; }
    for (int c; (c = fgetc(f)) != EOF;) song.push_back((uint8_t)c);
    fclose(f);
    std::vector<uint8_t> cmd = {'M', (uint8_t)song.size(), (uint8_t)(song.size() >> 8),
                                (uint8_t)(song.size() >> 16), (uint8_t)(song.size() >> 24)};

    auto *dut = new Vboard_wrap;
    static SdramModel sdram;
    std::string text;
    int rx_timer = -1, rx_n = 0, rx_byte = 0, tx_bit = -1, tx_timer = 0, tx_byte = 0;
    size_t tx_next = 0;
    bool sent = false, playing = false;
    double rc_l = 0, rc_r = 0, alpha = 1 - exp(-1 / (50e6 * 3300 * 4.7e-9));
    double min_l = 1, max_l = -1, min_r = 1, max_r = -1;
    long long play_from = 0;
    dut->SW = 0xF;            // all switches off (pulled up)
    dut->FTDI_D0 = 1;

    for (long long cycle = 0; cycle < 40000000; cycle++) {
        if (!sent && text.find("ready") != std::string::npos) {
            sent = true;
            for (size_t i = 0; i < song.size(); i++) {
                uint16_t &w = sdram.mem[i >> 1];
                w = (i & 1) ? (uint16_t)((w & 0x00FF) | song[i] << 8) : (uint16_t)((w & 0xFF00) | song[i]);
            }
        }
        if (sent && tx_bit < 0 && tx_next < cmd.size()) {
            tx_byte = cmd[tx_next++];
            tx_bit = 0;
            tx_timer = BIT;
        }
        if (tx_bit >= 0) {
            dut->FTDI_D0 = tx_bit == 0 ? 0 : tx_bit <= 8 ? (tx_byte >> (tx_bit - 1)) & 1 : 1;
            if (--tx_timer == 0) { tx_timer = BIT; if (++tx_bit == 10) tx_bit = -1; }
        }

        // read data is valid around our falling edge only (see sdram_model.h)
        dut->dq_from_sdram = sdram.dq_in;
        dut->dq_sdram_drives = sdram.driving;
        dut->CLK50 = 0;
        dut->eval();
        if (dut->SDRAM_CLK != 1) { printf("FAIL: SDRAM clock is not the inverse of the system clock\n"); return 1; }
        sdram.step(dut->SDRAM_CKE, dut->SDRAM_CSn, dut->SDRAM_RASn, dut->SDRAM_CASn, dut->SDRAM_WEn,
                   dut->SDRAM_BA, dut->SDRAM_A, dut->SDRAM_LDQM | dut->SDRAM_UDQM << 1,
                   dut->dq_from_fpga, dut->dq_fpga_drives);
        dut->dq_from_sdram = sdram.dq_garbage();
        dut->dq_sdram_drives = sdram.driving;
        dut->CLK50 = 1;
        dut->eval();
        if (sdram.failures) { printf("serial: %s\nFAIL: SDRAM rules broken\n", text.c_str()); return 1; }

        rc_l += ((dut->AUDIO2 ? 1.0 : -1.0) - rc_l) * alpha;
        rc_r += ((dut->AUDIO1 ? 1.0 : -1.0) - rc_r) * alpha;
        if (playing && cycle > play_from) {
            if (rc_l < min_l) min_l = rc_l;
            if (rc_l > max_l) max_l = rc_l;
            if (rc_r < min_r) min_r = rc_r;
            if (rc_r > max_r) max_r = rc_r;
            if (cycle > play_from + 5000000) {   // 0.1 s of sound
                bool ok = max_l - min_l > 0.2 && max_r - min_r < 0.01;
                printf("serial: %s\nleft pin swings %.3f, right pin swings %.3f (filtered): %s\n",
                       text.c_str(), max_l - min_l, max_r - min_r, ok ? "ok" : "FAIL");
                return ok ? 0 : 1;
            }
        }

        if (rx_timer < 0) {
            if (!dut->FTDI_D1) { rx_timer = BIT + BIT / 2; rx_n = 0; rx_byte = 0; }
        } else if (--rx_timer == 0) {
            if (rx_n < 8) { rx_byte |= (dut->FTDI_D1 & 1) << rx_n++; rx_timer = BIT; }
            else {
                text += (char)rx_byte;
                rx_timer = -1;
                if (!playing && text.find("OK ") != std::string::npos && text.back() == '\n') {
                    playing = true;
                    play_from = cycle + 500000;  // let the filters settle
                }
                if (text.find("ERR") != std::string::npos && text.back() == '\n') {
                    printf("serial: %s\nFAIL\n", text.c_str());
                    return 1;
                }
            }
        }
    }
    printf("serial: %s\nFAIL: timed out\n", text.c_str());
    return 1;
}
