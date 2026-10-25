// The HDMI audio chain end to end (ADR-0005, ADR-0010): Paula's frames for
// a Song (from the Replayer and the Paula C model) go into hdmi_out on the
// 50 MHz system clock; its TMDS output, on the 25.2 MHz pixel clock, is
// decoded by the sink model (hdmi_sink.h), and the audio must be the
// Resampler C model's output for the same frames, sample for sample:
// silence until the FIFO has primed, then every sample, none lost, added or
// changed. The FIFO must never run dry or overflow.
//
// Clocks run at their exact ratio (50 MHz : 25.2 MHz = 125 : 63). Frames
// arrive at Paula's average rate (3 546 895 per 50 million clocks) but, as
// on the board, bunched: Paula catches up after memory waits, here random
// stalls of up to 60 us (its queue allows 72 us).
//
//   hdmi_chain_tb song.mod seconds options
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include "Vhdmi_out.h"
#include "verilated.h"

#include "mod.h"
#include "paula_model.h"
#include "replayer.h"
#include "resampler.h"
#include "hdmi_sink.h"

static std::deque<std::pair<int32_t, int32_t>> expected;
static bool started;
static uint64_t matched, leading_silence;

static void hdmi_on_sample(int32_t l, int32_t r)
{
    if (!started) {
        if (l == 0 && r == 0) { leading_silence++; return; }
        while (!expected.empty() && expected.front().first == 0 && expected.front().second == 0)
            expected.pop_front();                 // the Song's own leading silence
        started = true;
    }
    if (expected.empty()) {
        FAIL("HDMI sample %llu that the model hasn't produced yet", (unsigned long long)matched);
        return;
    }
    auto e = expected.front();
    expected.pop_front();
    if (e.first != l || e.second != r)
        FAIL("sample %llu: HDMI (%d, %d), model (%d, %d)", (unsigned long long)matched, l, r, e.first, e.second);
    matched++;
}

// Nothing writes the text console here: every frame is its power-on screen,
// blank and black.
static int pictures;
static void hdmi_on_frame(const std::vector<uint32_t> &rgb)
{
    for (size_t i = 0; i < rgb.size(); i++)
        if (rgb[i] != 0) {
            FAIL("pixel (%zu, %zu) = %06x on a blank screen", i % 640, i / 640, rgb[i]);
            break;
        }
    pictures++;
}

#define NULL_SAMPLE_BYTES (65536u * 2)

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc != 4) {
        fprintf(stderr, "usage: hdmi_chain_tb song.mod seconds options\n");
        return 2;
    }
    double seconds = atof(argv[2]);
    uint32_t options = (uint32_t)atoi(argv[3]);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot read %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t null_addr = (size + 1) & ~1u;
    std::vector<uint8_t> mem(null_addr + NULL_SAMPLE_BYTES);
    if (fread(mem.data(), 1, size, f) != size) { fclose(f); return 2; }
    fclose(f);
    static mod_song_t song;
    if (mod_load(&song, mem.data(), size) != MOD_OK) { printf("bad MOD file\n"); return 2; }
    static paula_model_t paula;
    static replayer_t rp;
    paula_model_reset(&paula, mem.data(), (uint32_t)mem.size());
    paula_model_write(&paula, PAULA_OPTIONS, options);
    replayer_start(&rp, &song, mem.data(), 0, null_addr, paula_model_port(&paula));
    static resampler_t rs;
    resampler_reset(&rs);
    uint32_t tick_left = 0;

    Vhdmi_out *dut = new Vhdmi_out;
    dut->rst = 1; dut->rst_pixel = 1;
    for (int i = 0; i < 4; i++) {
        dut->clk = 1; dut->clk_pixel = 1; dut->eval();
        dut->clk = 0; dut->clk_pixel = 0; dut->eval();
    }
    dut->rst = 0; dut->rst_pixel = 0;

    // time in units of 1 / (50 MHz * 63): system clock every 63, pixel clock every 125
    uint64_t t_sys = 0, t_pix = 0, sys_cycles = 0;
    uint64_t end_sys = (uint64_t)(seconds * 50e6);
    uint64_t phase = 0, pending = 0, max_pending = 0;
    int cooldown = 0, stall = 0;
    uint32_t rng = 7;
    auto rnd = [&rng]() { rng = rng * 1103515245u + 12345u; return rng >> 8; };

    while (sys_cycles < end_sys) {
        if (t_sys <= t_pix) {
            // Paula: a frame falls due 3 546 895 times per 50 million clocks
            phase += 3546895;
            if (phase >= 50000000) { phase -= 50000000; pending++; }
            if (pending > max_pending) max_pending = pending;
            if (stall > 0) stall--;
            else if (pending < 20 && rnd() % 4000 == 0) stall = (int)(rnd() % 3000);
            dut->audio_valid = 0;
            if (cooldown > 0) cooldown--;
            if (pending > 0 && stall == 0 && cooldown == 0) {
                if (tick_left == 0) tick_left = rp.stopped ? 70940 : replayer_tick(&rp);
                tick_left--;
                paula_frame_t fr = paula_model_step(&paula);
                int32_t y[2];
                if (resampler_push(&rs, fr.left, fr.right, y)) expected.push_back({y[0], y[1]});
                dut->audio_valid = 1;
                dut->audio_l = (uint16_t)fr.left;
                dut->audio_r = (uint16_t)fr.right;
                pending--;
                cooldown = 3;                     // one frame every 3 clocks at most (Paula takes 4)
            }
            dut->clk = 1; dut->eval();
            dut->clk = 0; dut->eval();
            t_sys += 63;
            sys_cycles++;
        } else {
            dut->clk_pixel = 1; dut->eval();
            dut->clk_pixel = 0; dut->eval();
            t_pix += 125;
            sink(dut->tmds0, dut->tmds1, dut->tmds2);
            clk_no++;
        }
    }

    if (pictures < (int)(seconds * 60) - 2) FAIL("only %d whole pictures seen", pictures);
    if (dut->resampler_overrun) FAIL("Resampler overrun");
    if (dut->fifo_overflow) FAIL("FIFO overflow");
    if (dut->fifo_underflow) FAIL("FIFO ran dry");
    if (max_pending > 255) FAIL("test stalled Paula more than its queue allows (%llu)", (unsigned long long)max_pending);
    uint64_t want = (uint64_t)(seconds * 48000) - 100;   // less the FIFO and filter latency
    if (matched < want) FAIL("only %llu samples matched, expected about %llu", (unsigned long long)matched, (unsigned long long)want);
    if (expected.size() > 40) FAIL("%zu model samples never came out", expected.size());
    printf("%s %gs options %u: %s, %llu samples (after %llu of silence), longest Paula backlog %llu frames%s%s%s\n",
           argv[1], seconds, options, failures ? "FAILED" : "ok", (unsigned long long)matched,
           (unsigned long long)leading_silence, (unsigned long long)max_pending,
           dut->resampler_overrun ? ", Resampler overrun" : "", dut->fifo_overflow ? ", FIFO overflow" : "",
           dut->fifo_underflow ? ", FIFO ran dry" : "");
    delete dut;
    return failures ? 1 : 0;
}
