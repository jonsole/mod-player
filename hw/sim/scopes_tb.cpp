// The scopes' ring of capture slots on its own (ADR-0013): Paula's frames arrive
// with random long stalls (a paused Song stops them), so captures end at
// any point in a video frame; and the frame that finishes each capture is
// held back to land 0-23 clocks before the screen picks the slot it shows
// next, the moment where a careless hand-over would give one of the
// screen's slots to the next capture. A capture must never write a slot
// the screen is showing (the newest and the two before it), and captures
// must keep coming.
//
// Clocks run at their exact ratio (50 MHz : 25.2 MHz = 125 : 63); the
// testbench plays hdmi_tx's part by stepping pixel_x / pixel_y.
#include <cstdio>
#include <cstdlib>
#include "Vscopes.h"
#include "verilated.h"

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int frames = argc > 1 ? atoi(argv[1]) : 60;
    Vscopes *dut = new Vscopes;
    uint32_t rng = 5;
    auto rnd = [&rng]() { rng = rng * 1103515245u + 12345u; return rng >> 8; };
    int failures = 0, swaps = 0, last_front = -1, stall = 0;
    uint64_t writes = 0, phase = 0, t_sys = 0, t_pix = 0, cycles = 0;
    uint32_t x = 0, y = 0, frames_seen = 0, wave = 0;
    int in_capture = 0, captures = 0;            // points written in this capture; captures started
    bool holding = false;
    uint64_t release_at = 0, held = 0;

    dut->rst = 1; dut->rst_pixel = 1; dut->enable = 1;
    while ((int)frames_seen < frames) {
        if (t_sys <= t_pix) {
            if (cycles == 10) { dut->rst = 0; dut->rst_pixel = 0; }
            // a frame every 14 clocks or so, except during stalls (about one in 60 ms) of up to 1.5 video frames
            dut->audio_valid = 0;
            if (holding) {
                // the frame that finishes a capture, held back to land just before
                // the screen picks the slot it will show next
                if (t_sys >= release_at) { dut->audio_valid = 1; dut->voices = held; holding = false; }
            } else if (stall > 0) stall--;
            else if (rnd() % 3000000 == 0) stall = (int)(rnd() % 1250000);
            else if ((phase += 3546895) >= 50000000) {
                phase -= 50000000;
                dut->audio_valid = 1;
                wave++;
                uint64_t v = 0;
                for (int k = 0; k < 4; k++) {
                    int32_t s = (int32_t)((wave * (k + 3)) % 400) * 20 - 4000;   // sawtooths
                    v |= (uint64_t)(uint16_t)s << (16 * k);
                }
                dut->voices = v;
                if (in_capture == 4 * 155 - 1) {         // would this frame finish the capture?
                    dut->clk = 0; dut->eval();
                    if (dut->dbg_we) {
                        uint64_t pixels_left = (480ULL * 800 + 420000 - (y * 800ULL + x)) % 420000;
                        uint64_t t_latch = t_pix + (pixels_left ? pixels_left : 420000) * 125;
                        uint64_t early = (uint64_t)(captures % 24) * 63;   // 0..23 clocks early
                        release_at = t_latch > early ? t_latch - early : 0;
                        held = v;
                        holding = true;
                        dut->audio_valid = 0;
                    }
                }
            }
            dut->clk = 0; dut->eval();
            if (dut->dbg_start) { in_capture = 0; captures++; }
            for (int k = 0; k < 4; k++) in_capture += dut->dbg_we >> k & 1;
            if (dut->dbg_we && ((dut->dbg_front - dut->dbg_slot) & 3) < 3 && failures++ < 5)
                printf("FAIL: a capture wrote slot %d while the screen showed it (newest on screen: %d)\n",
                       dut->dbg_slot, dut->dbg_front);
            if (dut->dbg_we) writes++;
            dut->clk = 1; dut->eval();
            t_sys += 63;
            cycles++;
        } else {
            dut->pixel_x = x; dut->pixel_y = y;
            dut->clk_pixel = 1; dut->eval();
            dut->clk_pixel = 0; dut->eval();
            if ((int)dut->dbg_front != last_front) { if (last_front >= 0) swaps++; last_front = dut->dbg_front; }
            if (++x == 800) { x = 0; if (++y == 525) { y = 0; frames_seen++; } }
            t_pix += 125;
        }
    }
    if (swaps < frames * 6 / 10) {      // about 70% with the stalls; a capture every frame otherwise
        failures++;
        printf("FAIL: only %d hand-overs in %d frames: captures should keep up with the screen\n", swaps, frames);
    }
    printf("scopes slots: %s, %d frames, %d hand-overs, %llu points written\n", failures ? "FAILED" : "ok",
           frames, swaps, (unsigned long long)writes);
    delete dut;
    return failures ? 1 : 0;
}
