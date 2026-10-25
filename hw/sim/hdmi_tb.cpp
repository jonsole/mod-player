// HDMI transmitter test (ADR-0010): decodes hdmi_tx's TMDS characters the
// way a sink does and checks them against HDMI 1.3 / CEA-861 rules.
//
//  * Every character is a legal control, guard band, TERC4 or video code
//    for the period it is in; preambles are 8 characters, control periods
//    at least 12; Data Islands hold 1..18 packets with guard bands.
//  * Timing is 640x480p60: 800 x 525 clocks, HSYNC 96 clocks and VSYNC
//    2 lines (both negative, VSYNC starting with HSYNC), video 640 x 480
//    starting 144 clocks after HSYNC and 35 lines after VSYNC starts
//    (34 HSYNC edges later: line 0 starts before its own HSYNC).
//  * Every pixel is the expected colour; video characters keep the DC
//    balance (running disparity bounded).
//  * Every packet's BCH ECC is right; only null, ACR, audio sample, AVI
//    and Audio InfoFrame packets appear; InfoFrame checksums and fields
//    are right and both are sent every frame; ACR has N = 6144,
//    CTS = 25200 and comes every 25 200 clocks on average.
//  * Audio: the samples fed in come out in order, none lost or added,
//    one per 525 clocks; parity, B flags and the channel status block
//    (PCM, 48 kHz, 24-bit, channels 1 and 2) are right.
//
//   hdmi_tb frames
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include "Vhdmi_tx.h"
#include "verilated.h"

#include "hdmi_sink.h"

// ---- the audio we feed in ----------------------------------------------------------
struct Sample { int32_t l, r; };
static std::deque<Sample> sent;
static uint64_t samples_in;
static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void hdmi_on_sample(int32_t l, int32_t r)
{
    if (sent.empty()) {
        FAIL("audio sample out that was never fed in");
        return;
    }
    Sample e = sent.front();
    sent.pop_front();
    if (e.l != l || e.r != r)
        FAIL("audio sample %llu: got (%d, %d), fed (%d, %d)", (unsigned long long)samples_out, l, r, e.l, e.r);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int frames = argc > 1 ? atoi(argv[1]) : 3;
    Vhdmi_tx *dut = new Vhdmi_tx;
    dut->rst = 1;
    for (int i = 0; i < 4; i++) { dut->clk = 0; dut->eval(); dut->clk = 1; dut->eval(); }
    dut->rst = 0;
    uint64_t total = (uint64_t)frames * 800 * 525 + 1000;
    uint64_t first_tick = 0;
    for (clk_no = 0; clk_no < total; clk_no++) {
        dut->clk = 0;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        if (dut->audio_tick) {
            // the sample for this tick, taken at the next edge
            Sample s = {(int32_t)(rnd() & 0xFFFFFF) - 0x800000, (int32_t)(rnd() & 0xFFFFFF) - 0x800000};
            if (samples_in == 0) { s.l = 0x7FFFFF; s.r = -0x800000; }   // extremes too
            dut->audio_l = (uint32_t)s.l & 0xFFFFFF;
            dut->audio_r = (uint32_t)s.r & 0xFFFFFF;
            sent.push_back(s);
            if (samples_in == 0) first_tick = clk_no;
            samples_in++;
        }
        sink(dut->tmds0, dut->tmds1, dut->tmds2);
    }
    uint64_t ticks_expected = (total - first_tick - 1) / 525 + 1;
    if (samples_in != ticks_expected) { clk_no = total; FAIL("%llu audio ticks, expected %llu", (unsigned long long)samples_in, (unsigned long long)ticks_expected); }
    if (sent.size() > 4) { clk_no = total; FAIL("%zu samples fed in but never sent", sent.size()); }
    if (frame_checks < frames - 1) { clk_no = total; FAIL("only %d whole frames seen", frame_checks); }
    double acr_period = acr_count > 1 ? (double)(acr_last_clk - acr_first_clk) / (acr_count - 1) : 0;
    if (acr_count < 2 || acr_period < 25150 || acr_period > 25250) { clk_no = total; FAIL("ACR every %.0f clocks (%llu sent)", acr_period, (unsigned long long)acr_count); }
    printf("hdmi_tx: %s, %d frames, %llu audio samples, %llu ACR (every %.1f clocks)\n",
           failures ? "FAILED" : "ok", frame_checks, (unsigned long long)samples_out,
           (unsigned long long)acr_count, acr_period);
    delete dut;
    return failures ? 1 : 0;
}
