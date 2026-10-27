// The scopes' ring of capture slots on its own (ADR-0013). A capture must
// never write a slot the screen is showing (the newest and the two before
// it), and captures must keep coming.
//
// The risky moment is when a capture finishes at almost the same time as the
// screen picks the slot it will show next: a careless hand-over would then
// give one of the screen's slots to the next capture. To land captures on
// that moment, the Voices are constant (so the capture's timing is exactly
// repeatable: it triggers on the time-out and takes a knot every 1100
// frames), and a few columns before the end of each capture the frames are
// held back until a time computed to put the capture's last write between
// -8 and +8 clocks of the screen's pick, a different offset each capture. The
// computation corrects itself from where the previous capture ended.
//
// Clocks run at their exact ratio (50 MHz : 25.2 MHz = 125 : 63); the
// testbench plays hdmi_tx's part by stepping pixel_x / pixel_y. Times are in
// units of 1 / (50 MHz * 63): 63 to a system clock, 125 to a pixel clock.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "Vscopes.h"
#include "verilated.h"

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int frames = argc > 1 ? atoi(argv[1]) : 60;
    Vscopes *dut = new Vscopes;
    int failures = 0, swaps = 0, last_front = -1;
    uint64_t writes = 0, phase = 0, t_sys = 0, t_pix = 0, cycles = 0;
    uint32_t x = 0, y = 0, frames_seen = 0;
    int in_capture = 0, captures = 0;            // points written in this capture; captures started
    bool holding = false, hold_taken = false, done_seen = false;
    uint64_t release_at = 0, copy_used = 0;
    int64_t bias = 0;                            // clocks, corrected from the last capture
    int worst_early = 1000, worst_late = -1000;  // where captures ended, in clocks from the screen's pick
    const uint64_t VIDEO_FRAME = 420000ULL * 125;
    const int64_t EST = 2 * 1100 * 14 + 700;     // clocks from the hold to the last write (a first guess)

    dut->rst = 1; dut->rst_pixel = 1; dut->enable = 1;
    dut->voices = (uint64_t)6000 << 48 | (uint64_t)5000 << 32 | (uint64_t)4000 << 16 | 3000;
    while ((int)frames_seen < frames) {
        if (t_sys <= t_pix) {
            if (cycles == 10) { dut->rst = 0; dut->rst_pixel = 0; }
            // a frame every 14 clocks or so (Paula's rate), unless held back
            dut->audio_valid = 0;
            if (holding) {
                if (t_sys >= release_at) holding = false;
            } else if ((phase += 3546895) >= 50000000) {
                phase -= 50000000;
                dut->audio_valid = 1;
            }
            dut->clk = 0; dut->eval();
            if (dut->dbg_start) { in_capture = 0; captures++; hold_taken = done_seen = false; }
            for (int k = 0; k < 4; k++) in_capture += dut->dbg_we >> k & 1;
            if (dut->dbg_we && ((dut->dbg_front - dut->dbg_slot) & 3) < 3 && failures++ < 5)
                printf("FAIL: a capture wrote slot %d while the screen showed it (newest on screen: %d)\n",
                       dut->dbg_slot, dut->dbg_front);
            if (dut->dbg_we) writes++;
            if (in_capture >= 4 * 308 && !hold_taken) {
                // nearly done: hold the frames until the end will land where this capture's turn is
                hold_taken = true;
                uint64_t pixels_left = (480ULL * 800 + 420000 - (y * 800ULL + x)) % 420000;
                uint64_t copy = t_pix + (pixels_left ? pixels_left : 420000) * 125;
                int64_t want = (captures % 17) - 8;     // -8..8 clocks from the pick
                int64_t lead = EST * 63 - (bias + want) * 63;
                while ((int64_t)copy - lead < (int64_t)t_sys + 100 * 63) copy += VIDEO_FRAME;
                release_at = (uint64_t)((int64_t)copy - lead);
                copy_used = copy;
                holding = true;
                dut->audio_valid = 0;
            }
            if (in_capture == 4 * 316 && !done_seen) {
                done_seen = true;
                int64_t m = ((int64_t)t_sys - (int64_t)copy_used) / 63;   // clocks after the pick
                int64_t want = (captures % 17) - 8;
                bias += want - m;                       // so the next lands where it should
                if (captures >= 10) {                   // once the correction has settled
                    if (m < worst_early) worst_early = (int)m;
                    if (m > worst_late) worst_late = (int)m;
                }
            }
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
    if (swaps < frames / 5) {
        failures++;
        printf("FAIL: only %d hand-overs in %d frames: captures should keep coming\n", swaps, frames);
    }
    if (captures > 30 && (worst_early > -4 || worst_late < 4)) {
        failures++;
        printf("FAIL: captures ended only from %d to %d clocks after the screen's pick: the "
               "test didn't reach the moment it is for\n", worst_early, worst_late);
    }
    printf("scopes slots: %s, %d frames, %d hand-overs, %d captures, %llu points written, "
           "ended %d to %d clocks from the screen's pick\n", failures ? "FAILED" : "ok",
           frames, swaps, captures, (unsigned long long)writes, worst_early, worst_late);
    delete dut;
    return failures ? 1 : 0;
}
