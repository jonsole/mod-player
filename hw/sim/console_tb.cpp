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
//     --song FILE                the status screen (issue #22): when the firmware
//                                starts a Song (it writes Paula's options), run the
//                                PC reference Replayer on FILE alongside, stepped by
//                                the chip's audio frames (which must match it), and
//                                check every frame's "Pos ... BPM" line against the
//                                rows the reference played in the frame and the two
//                                before it (the screen follows within two frames);
//                                at the end, every row that lasted 4 frames or more
//                                must have been shown, and the Song's name must be
//                                on the screen. The Voice scopes (issue #23) too:
//                                every point captured must be its Voice's output in
//                                the reference >> 7 times 0.75, taken at a rising zero crossing
//                                (or after the time-out) and then every 64 colour
//                                clocks; and every frame's scope pixels must be the
//                                traces drawn from the slots on screen: the newest
//                                capture bright, the two before at half and a quarter
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
#include "mod.h"
#include "paula_model.h"
#include "replayer.h"

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

// ---- the status screen check (--song) -----------------------------------------------
struct RowState { int pos, pattern, row, speed, bpm; uint64_t from; };

struct Reference {
    std::vector<uint8_t> mem;
    mod_song_t song;
    paula_model_t paula;
    replayer_t rp;
    std::deque<paula_frame_t> frames;
    std::deque<std::vector<int32_t>> voice_frames;   // each Voice's part of each frame
    int32_t voices[4] = {0, 0, 0, 0}, prev_voices[4] = {0, 0, 0, 0};
    std::vector<RowState> rows;            // what was heard, from when
    bool started = false;
    uint64_t audio_frames = 0;

    void start(const std::vector<uint8_t> &file, uint32_t options)
    {
        uint32_t silence = ((uint32_t)file.size() + 3) & ~3u;
        mem.assign(silence + 131072 + 4, 0);
        std::copy(file.begin(), file.end(), mem.begin());
        if (mod_load(&song, mem.data(), (uint32_t)file.size()) != MOD_OK) { printf("reference: not a MOD file\n"); exit(2); }
        paula_model_reset(&paula, mem.data(), (uint32_t)mem.size());
        paula_model_write(&paula, PAULA_OPTIONS, options);
        replayer_start(&rp, &song, mem.data(), 0, silence, paula_model_port(&paula));
        frames.clear();
        voice_frames.clear();
        rows.clear();
        started = true;
    }
    // The next audio frame; notes the row being heard when a tick starts.
    paula_frame_t next(uint64_t now)
    {
        while (frames.empty()) {
            uint32_t cck = replayer_tick(&rp);
            RowState s = {rp.play_pos, song.order[rp.play_pos], rp.play_row, rp.speed, rp.bpm, now};
            if (rows.empty() || s.pos != rows.back().pos || s.row != rows.back().row ||
                s.speed != rows.back().speed || s.bpm != rows.back().bpm)
                rows.push_back(s);
            for (uint32_t i = 0; i < cck; i++) {
                std::vector<int32_t> v(4);
                for (int k = 0; k < 4; k++) v[k] = paula.voice[k].on ? paula.voice[k].out : 0;
                voice_frames.push_back(v);
                frames.push_back(paula_model_step(&paula));
            }
        }
        paula_frame_t f = frames.front();
        frames.pop_front();
        for (int k = 0; k < 4; k++) { prev_voices[k] = voices[k]; voices[k] = voice_frames.front()[k]; }
        voice_frames.pop_front();
        audio_frames++;
        return f;
    }
};
static Reference ref;
static std::vector<uint8_t> status_song;
static int status_frames;                   // frames whose status line was checked
static std::vector<int> rows_shown;         // per reference row: frames it was shown in

// the scopes: the testbench's copy of their buffers, and what it has checked
static int8_t scope_buf[4][4][512];        // Voice, slot, point
static bool scope_enable;
static int scope_front;
static uint64_t scope_enable_changed;      // pixel clock of the last change
static int scope_frames;
static uint64_t scope_trace_pixels, scope_points;
static uint64_t scope_wait_from[4], scope_last_frame[4];
static int scope_last_index[4] = {-1, -1, -1, -1};

// What the scopes draw at (x, y), 0 for nothing (hw/rtl/scopes.v): the
// newest capture's trace, else the one before at half brightness, else the
// one before that at a quarter, else the centre line.
static uint32_t scope_pixel(int x, int y)
{
    if (x < 2 || x >= 638 || y < 96 || y >= 288) return 0;
    int gx = x - 2, right = gx >= 320, col = right ? gx - 320 : gx, yl = y - 96;
    int v = (yl >= 96 ? 2 : 0) + right;            // a 2 x 2 grid, 316 x 96 each
    if (col >= 316) return 0;                      // the gap between scopes
    static const uint32_t colour[4] = {0x55FF55, 0x55FFFF, 0xFFFF55, 0xFF55FF};
    int dy = 48 - (yl >= 96 ? yl - 96 : yl);
    for (int age = 0; age < 3; age++) {
        int slot = (scope_front - age) & 3;
        int here = scope_buf[v][slot][col], before = col ? scope_buf[v][slot][col - 1] : here;
        int lo = here < before ? here : before, hi = here < before ? before : here;
        if (dy >= lo && dy <= hi)
            return age == 0 ? colour[v] : age == 1 ? (colour[v] >> 1) & 0x7F7F7F : (colour[v] >> 2) & 0x3F3F3F;
    }
    return dy == 0 ? 0x555555 : 0;
}

// the spectrum analyser's bars as the display copied them (hw/rtl/spectrum.v)
static uint8_t spectrum_h[48], spectrum_p[48];
static bool spectrum_enable;
static uint64_t spectrum_enable_changed;
static int spectrum_frames;
static uint64_t spectrum_pixels;

static uint32_t spectrum_pixel(int x, int y)
{
    if (x < 8 || x >= 632 || y < 288 || y >= 480) return 0;
    int bx = x - 8, bar = bx / 13, sub = bx % 13, pos = 479 - y;
    if (sub >= 12) return 0;
    if (spectrum_p[bar] && (pos == spectrum_p[bar] || pos == spectrum_p[bar] + 1)) return 0xE0E0E0;
    if (pos < spectrum_h[bar]) {
        int red = pos < 96 ? pos * 5 / 2 : 255, green = pos < 96 ? 255 : 255 - (pos - 96) * 5 / 2;
        return (uint32_t)(red << 16 | green << 8 | 0x20);
    }
    return 0;
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
    // the scopes, if they were on (or off) for the whole frame
    bool scope_steady = scope_enable_changed + 3 < start;
    bool scopes_shown = scope_steady && scope_enable;
    bool spectrum_steady = spectrum_enable_changed + 3 < start;
    bool spectrum_shown = spectrum_steady && spectrum_enable;
    if (spectrum_shown) {
        spectrum_frames++;
        for (int y = 288; y < 480; y++)
            for (int x = 8; x < 632; x++)
                if (spectrum_pixel(x, y)) spectrum_pixels++;
    }
    if (scopes_shown) {
        scope_frames++;
        for (int y = 96; y < 288; y++)
            for (int x = 2; x < 638; x++) {
                uint32_t s = scope_pixel(x, y);
                if (s && s != 0x555555) scope_trace_pixels++;
            }
    }
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
            bool in_band = row >= 4 && row < 12, in_spectrum = row >= 12;
            if (in_band && !scope_steady) continue;          // the scopes came or went mid-frame
            if (in_spectrum && !spectrum_steady) continue;   // the spectrum likewise
            bool ok = false;
            for (uint16_t v : held) {
                uint32_t want[12];
                render_row(v, r, want);
                if (in_band && scopes_shown)
                    for (int i = 0; i < CW; i++) {
                        uint32_t s = scope_pixel(LEFT + col * CW + i, row * CH + r);
                        if (s) want[i] = s;
                    }
                if (in_spectrum && spectrum_shown)
                    for (int i = 0; i < CW; i++) {
                        uint32_t s = spectrum_pixel(LEFT + col * CW + i, row * CH + r);
                        if (s) want[i] = s;
                    }
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

    // the status line, as the text RAM holds it at the end of the frame
    if (!ref.started || ref.rows.empty()) return;
    char line[54];
    for (int i = 0; i < 53; i++) line[i] = (char)(text_ram[2 * 53 + i] & 0xFF);
    line[53] = 0;
    int pos, len, pattern, row, speed, bpm;
    if (sscanf(line, " Pos %d/%d  Pat %d  Row %d  Spd %d  BPM %d", &pos, &len, &pattern, &row, &speed, &bpm) != 6)
        return;                                  // not drawn yet
    if (len != ref.song.length) FAIL("frame %d: status says %d Positions, the Song has %d", frames_checked, len, ref.song.length);
    // the reference rows heard from two frames before this one started, to its end
    uint64_t from = end > 3 * 420000ULL ? end - 3 * 420000ULL : 0;
    bool ok = false;
    for (size_t i = 0; i < ref.rows.size(); i++) {
        const RowState &s = ref.rows[i];
        uint64_t until = i + 1 < ref.rows.size() ? ref.rows[i + 1].from : end;
        if (until < from || s.from > end) continue;
        if (s.pos == pos && s.pattern == pattern && s.row == row && s.speed == speed && s.bpm == bpm) {
            ok = true;
            if (rows_shown.size() < ref.rows.size()) rows_shown.resize(ref.rows.size());
            rows_shown[i]++;
        }
    }
    if (!ok) {
        const RowState &s = ref.rows.back();
        FAIL("frame %d shows Pos %d Pat %d Row %d Spd %d BPM %d; the reference is at Pos %d Pat %d Row %d Spd %d BPM %d",
             frames_checked, pos, pattern, row, speed, bpm, s.pos, s.pattern, s.row, s.speed, s.bpm);
    }
    status_frames++;
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
        else if (!strcmp(argv[i], "--song") && i + 1 < argc) status_song = read_file(argv[++i]);
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
            if (dut->dbg_scope_start)
                for (int v = 0; v < 4; v++) scope_wait_from[v] = ref.audio_frames;
            if (!status_song.empty()) {
                if (dut->dbg_paula_we && dut->dbg_paula_addr == PAULA_OPTIONS)
                    ref.start(status_song, dut->dbg_paula_wdata);     // the firmware starts the Song
                if (ref.started && dut->audio_valid) {
                    paula_frame_t e = ref.next(clk_no);
                    // the scopes' capture: each point is its Voice's output >> 7, from
                    // a rising zero crossing (or the time-out), then every 128 frames
                    for (int v = 0; v < 4; v++) {
                        if (!(dut->dbg_scope_we >> v & 1)) continue;
                        int index = (int)(dut->dbg_scope_index >> (9 * v) & 0x1FF);
                        int8_t point = (int8_t)(dut->dbg_scope_points >> (8 * v) & 0xFF);
                        scope_buf[v][dut->dbg_scope_slot][index] = point;
                        scope_points++;
                        int want = (ref.voices[v] >> 7) - ((ref.voices[v] >> 7) >> 2);   // >> 7, times 0.75
                        if (point != want && failures++ < 20)
                            printf("FAIL: scope %d point %d is %d, the reference Voice is %d (so %d)\n",
                                   v + 1, index, point, ref.voices[v], want);
                        bool crossing = ref.prev_voices[v] < 0 && ref.voices[v] >= 0;
                        if (index == 0 ? !(crossing || ref.audio_frames - scope_wait_from[v] > 36000)
                                       : (index != scope_last_index[v] + 1 ||
                                          ref.audio_frames - scope_last_frame[v] != 64))
                            if (failures++ < 20)
                                printf("FAIL: scope %d point %d taken at the wrong time (frame %llu; last point %d "
                                       "at %llu)\n", v + 1, index, (unsigned long long)ref.audio_frames,
                                       scope_last_index[v], (unsigned long long)scope_last_frame[v]);
                        scope_last_index[v] = index;
                        scope_last_frame[v] = ref.audio_frames;
                    }
                    if ((int16_t)dut->audio_l != e.left || (int16_t)dut->audio_r != e.right)
                        if (failures++ < 20)
                            printf("FAIL: audio frame %llu: chip (%d, %d), reference (%d, %d)\n",
                                   (unsigned long long)ref.audio_frames, (int16_t)dut->audio_l,
                                   (int16_t)dut->audio_r, e.left, e.right);
                }
            }
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
            if (dut->dbg_spectrum_copy)          // the spectrum display took its copy at this edge
                for (int b = 0; b < 48; b++) {
                    spectrum_h[b] = (uint8_t)(dut->dbg_spectrum_bars[b / 4] >> (8 * (b % 4)));
                    spectrum_p[b] = (uint8_t)(dut->dbg_spectrum_peaks[b / 4] >> (8 * (b % 4)));
                }
            if (dut->dbg_text_we) {           // the console's text RAM takes a write at this edge
                uint16_t &v = text_ram[dut->dbg_text_cell];
                if (dut->dbg_text_be & 1) v = (uint16_t)((v & 0xFF00) | (dut->dbg_text_data & 0x00FF));
                if (dut->dbg_text_be & 2) v = (uint16_t)((v & 0x00FF) | (dut->dbg_text_data & 0xFF00));
                if (dut->dbg_text_cell < CELLS) history[dut->dbg_text_cell].push_back({clk_no, v});
            }
            dut->clk_pixel = 0;
            dut->eval();
            if ((bool)(dut->dbg_control & 1) != scope_enable) {
                scope_enable = dut->dbg_control & 1;
                scope_enable_changed = clk_no;
            }
            if ((bool)(dut->dbg_control & 2) != spectrum_enable) {
                spectrum_enable = dut->dbg_control & 2;
                spectrum_enable_changed = clk_no;
            }
            scope_front = dut->dbg_scope_front;
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
    if (!status_song.empty()) {
        // every row heard for 4 frames or more must have been shown
        int missed = 0;
        for (size_t i = 0; i + 1 < ref.rows.size(); i++) {
            uint64_t lasted = ref.rows[i + 1].from - ref.rows[i].from;
            if (lasted >= 4 * 420000ULL && (i >= rows_shown.size() || rows_shown[i] == 0) && missed++ < 5)
                FAIL("Pos %d Row %d played for %.1f frames but was never shown", ref.rows[i].pos, ref.rows[i].row,
                     lasted / 420000.0);
        }
        // the names
        auto row_text = [](int r, int c, int n) {
            std::string s;
            for (int i = 0; i < n; i++) s += (char)(text_ram[r * 53 + c + i] & 0xFF);
            while (!s.empty() && (s.back() == ' ' || s.back() == 0)) s.pop_back();
            return s;
        };
        if (ref.started) {
            if (row_text(1, 1, 20) != ref.song.name)
                FAIL("Song name shown as \"%s\", not \"%s\"", row_text(1, 1, 20).c_str(), ref.song.name);
        } else {
            FAIL("the firmware never started the Song");
        }
        printf("status: %d frames checked, %zu reference rows, %llu audio frames match\n", status_frames,
               ref.rows.size(), (unsigned long long)ref.audio_frames);
        printf("scopes: %d frames, %llu trace pixels, %llu points checked\n", scope_frames,
               (unsigned long long)scope_trace_pixels, (unsigned long long)scope_points);
        printf("spectrum: %d frames, %llu bar pixels\n", spectrum_frames, (unsigned long long)spectrum_pixels);
    }
    printf("console: %s, %d frames match the model (%d cell-frames caught mid-change), %llu clocks\n",
           failures ? "FAILED" : "ok", frames_checked, cells_seen_changing, (unsigned long long)sys_cycles);
    delete dut;
    return failures ? 1 : 0;
}
