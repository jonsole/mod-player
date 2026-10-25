// The HDMI text console end to end (issue #21, ADR-0012): the soft CPU runs
// the firmware, which draws its test screen; the HDMI output is decoded by
// the sink model (hdmi_sink.h) and every whole frame is checked, pixel for
// pixel, against a model of the console: the font file, the 16 colours,
// and the text RAM as the console's own writes left it (watched on its
// pixel-clock write port).
//
// Each 8-pixel row of each character cell must be that cell's glyph row in
// one of the values the cell held while the frame was being shown. So a
// write finished before a frame starts must show in that frame, and a cell
// never shows anything it didn't hold (no garbage at the clock crossing).
//
//   console_tb [options]
//     --after TEXT --send FILE   when TEXT appears on the serial output (after
//                                the previous step), send FILE (repeatable)
//     --expect TEXT              then wait for TEXT on the serial output
//     --frames N                 then check N more frames (default 2)
//     --ppm FILE                 write the last frame there (binary PPM)
//     --max N                    give up after N system clocks (default 100 million)
//
// Clocks run at their exact ratio (50 MHz : 25.2 MHz = 125 : 63).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "Vconsole_wrap.h"
#include "verilated.h"
#include "sdram_model.h"
#include "hdmi_sink.h"

static const int BIT = 50;                         // 50 MHz / 1 Mbaud

static std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> d;
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot read %s\n", path); exit(2); }
    int c;
    while ((c = fgetc(f)) != EOF) d.push_back((uint8_t)c);
    fclose(f);
    return d;
}

// ---- the console model ------------------------------------------------------------
static uint16_t font[8192];
static const uint32_t PALETTE[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

static void load_font(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("cannot read %s\n", path); exit(2); }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < 8192)
        if (line[0] != '/' && line[0] != '\n')
            font[n++] = (uint16_t)strtol(line, nullptr, 16);
    fclose(f);
    if (n != 8192) { printf("%s: %d font rows, expected 8192\n", path, n); exit(2); }
}

// The console: 53 x 20 cells of 12 x 24 pixels from x = 2 (a 2-pixel black
// border at each side).
static const int COLS = 53, ROWS = 20, CW = 12, CH = 24, LEFT = 2, CELLS = 53 * 20;

// The 12 pixels of glyph row r of a cell holding v (attribute << 8 | character).
static void render_row(uint16_t v, int r, uint32_t out[12])
{
    uint16_t bits = font[(v & 0xFF) * 32 + r];
    uint32_t fg = PALETTE[v >> 8 & 15], bg = PALETTE[v >> 12 & 15];
    for (int i = 0; i < CW; i++) out[i] = (bits >> (11 - i) & 1) ? fg : bg;
}

struct Change { uint64_t clk; uint16_t value; };
static std::vector<Change> history[53 * 20];          // per cell, oldest first; starts at 0
static uint16_t text_ram[4096];

static int frames_checked, cells_seen_changing;
static std::vector<uint32_t> last_picture;

static void hdmi_on_frame(const std::vector<uint32_t> &rgb)
{
    // this frame was read from the text RAM from about here (a little early, to be safe)
    uint64_t end = clk_no, start = end > 479 * 800 + 640 + 32 ? end - (479 * 800 + 640 + 32) : 0;
    int bad = 0;
    for (int y = 0; y < 480; y++)
        for (int x : {0, 1, 638, 639})
            if (rgb[y * 640 + x] != 0 && bad++ < 5) FAIL("border pixel (%d, %d) = %06x", x, y, rgb[y * 640 + x]);
    for (int cell = 0; cell < CELLS; cell++) {
        // the values this cell held during the frame
        std::vector<uint16_t> held;
        uint16_t before = 0;
        for (const Change &c : history[cell]) {
            if (c.clk <= start) before = c.value;
            else if (c.clk <= end) held.push_back(c.value);
        }
        held.insert(held.begin(), before);
        if (held.size() > 1) cells_seen_changing++;
        int row = cell / COLS, col = cell % COLS;
        for (int r = 0; r < CH; r++) {
            const uint32_t *got = &rgb[(row * CH + r) * 640 + LEFT + col * CW];
            bool ok = false;
            for (uint16_t v : held) {
                uint32_t want[12];
                render_row(v, r, want);
                if (!memcmp(got, want, sizeof want)) { ok = true; break; }
            }
            if (!ok && bad++ < 5)
                FAIL("frame %d: cell %d (row %d, column %d) glyph row %d is none of the %zu value(s) "
                     "it held (last %04x): first pixels %06x %06x %06x %06x", frames_checked,
                     cell, row, col, r, held.size(), held.back(), got[0], got[1], got[2], got[3]);
        }
        // forget changes no later frame can show
        while (history[cell].size() > 1 && history[cell][1].clk <= start)
            history[cell].erase(history[cell].begin());
    }
    frames_checked++;
    last_picture = rgb;
}

static void hdmi_on_sample(int32_t, int32_t) {}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    struct Step { const char *after; std::vector<uint8_t> send; };
    std::vector<Step> steps;
    const char *expect = nullptr, *ppm = nullptr, *font_path = nullptr;
    int frames_after = 2;
    uint64_t max_sys = 100000000ULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--after") && i + 1 < argc) steps.push_back({argv[++i], {}});
        else if (!strcmp(argv[i], "--send") && i + 1 < argc && !steps.empty()) steps.back().send = read_file(argv[++i]);
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc) expect = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max_sys = strtoull(argv[++i], nullptr, 0);
        else { printf("unknown option %s\n", argv[i]); return 2; }
    }
    if (!font_path) { printf("needs --font console_font.hex\n"); return 2; }
    load_font(font_path);

    Vconsole_wrap *dut = new Vconsole_wrap;
    static SdramModel sdram;
    std::string text;
    int rx_timer = -1, rx_index = 0, rx_byte = 0;
    std::deque<uint8_t> tx_queue;
    int tx_timer = 0, tx_bit = -1, tx_byte = 0;
    size_t step = 0, acted_at = 0;
    bool done_serial = false;
    int frames_at_done = 0;

    dut->rst = 1; dut->rst_pixel = 1; dut->uart_rx = 1;
    uint64_t t_sys = 0, t_pix = 0, sys_cycles = 0;
    while (sys_cycles < max_sys) {
        if (t_sys <= t_pix) {
            if (sys_cycles == 10) { dut->rst = 0; dut->rst_pixel = 0; }
            if (step < steps.size() && text.find(steps[step].after, acted_at) != std::string::npos) {
                tx_queue.insert(tx_queue.end(), steps[step].send.begin(), steps[step].send.end());
                acted_at = text.size();
                step++;
            }
            if (!done_serial && step == steps.size() &&
                (!expect || text.find(expect, acted_at) != std::string::npos)) {
                done_serial = true;
                frames_at_done = frames_checked;
            }
            if (done_serial && frames_checked >= frames_at_done + frames_after)
                break;

            // serial input
            if (tx_bit < 0 && !tx_queue.empty()) {
                tx_byte = tx_queue.front(); tx_queue.pop_front();
                tx_bit = 0; tx_timer = BIT;
            }
            if (tx_bit >= 0) {
                dut->uart_rx = tx_bit == 0 ? 0 : tx_bit <= 8 ? (tx_byte >> (tx_bit - 1)) & 1 : 1;
                if (--tx_timer == 0) { tx_timer = BIT; if (++tx_bit == 10) tx_bit = -1; }
            } else {
                dut->uart_rx = 1;
            }

            dut->sd_dq_i = sdram.dq_in;
            dut->clk = 0;
            dut->eval();
            sdram.step(dut->sd_cke, dut->sd_cs_n, dut->sd_ras_n, dut->sd_cas_n, dut->sd_we_n,
                       dut->sd_ba, dut->sd_a, dut->sd_dqm, dut->sd_dq_o, dut->sd_dq_oe);
            dut->sd_dq_i = sdram.dq_garbage();
            dut->clk = 1;
            dut->eval();
            if (sdram.failures) { printf("serial: %s\nFAIL: SDRAM rules broken\n", text.c_str()); return 1; }

            // serial output
            if (rx_timer < 0) {
                if (!dut->uart_tx) { rx_timer = BIT + BIT / 2; rx_index = 0; rx_byte = 0; }
            } else if (--rx_timer == 0) {
                if (rx_index < 8) { rx_byte |= (dut->uart_tx & 1) << rx_index++; rx_timer = BIT; }
                else { text += (char)rx_byte; rx_timer = -1; }
            }
            t_sys += 63;
            sys_cycles++;
        } else {
            dut->clk_pixel = 1;
            dut->eval();
            if (dut->dbg_text_we) {           // the console's text RAM takes a write at this edge
                uint16_t &v = text_ram[dut->dbg_text_cell];
                if (dut->dbg_text_be & 1) v = (uint16_t)((v & 0xFF00) | (dut->dbg_text_data & 0x00FF));
                if (dut->dbg_text_be & 2) v = (uint16_t)((v & 0x00FF) | (dut->dbg_text_data & 0xFF00));
                if (dut->dbg_text_cell < CELLS) history[dut->dbg_text_cell].push_back({clk_no, v});
            }
            dut->clk_pixel = 0;
            dut->eval();
            sink(dut->tmds0, dut->tmds1, dut->tmds2);
            clk_no++;
            t_pix += 125;
        }
    }
    if (!done_serial || frames_checked < frames_at_done + frames_after)
        FAIL("ran out of time (%llu clocks): %d frames checked, serial: %s", (unsigned long long)sys_cycles,
             frames_checked, text.c_str());
    if (ppm && !last_picture.empty()) {
        FILE *f = fopen(ppm, "wb");
        fprintf(f, "P6\n640 480\n255\n");
        for (uint32_t px : last_picture) { fputc(px >> 16, f); fputc(px >> 8 & 0xFF, f); fputc(px & 0xFF, f); }
        fclose(f);
    }
    printf("serial: %s\n", text.c_str());
    printf("console: %s, %d frames match the model (%d cell-frames caught mid-change), %llu clocks\n",
           failures ? "FAILED" : "ok", frames_checked, cells_seen_changing, (unsigned long long)sys_cycles);
    delete dut;
    return failures ? 1 : 0;
}
