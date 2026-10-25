// An HDMI sink model for testbenches (ADR-0010): decodes hdmi_tx's TMDS
// characters, one call to sink() per pixel clock, and checks them against
// HDMI 1.3 / CEA-861 rules (see hdmi_tb.cpp for the list). Decoded audio
// samples go to hdmi_on_sample(), which the testbench defines; failures are
// counted in `failures`. Include once per testbench.
#ifndef HDMI_SINK_H
#define HDMI_SINK_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const uint32_t COLOUR = 0x0055AA;   // must match hdmi_tx's default

static int failures;
static uint64_t clk_no;                     // the testbench counts pixel clocks
#define FAIL(...) do { if (failures++ < 20) { printf("FAIL @%llu: ", (unsigned long long)clk_no); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- character tables (HDMI 1.3 section 5.4) ----------------------------------
static const uint16_t TERC4[16] = {
    0x29C, 0x263, 0x2E4, 0x2E2, 0x171, 0x11E, 0x18E, 0x13C,
    0x2CC, 0x139, 0x19C, 0x2C6, 0x28E, 0x271, 0x163, 0x2C3};
static const uint16_t CTRL[4] = {0x354, 0x0AB, 0x154, 0x2AB};
static const uint16_t VGB_02 = 0x2CC, VGB_1 = 0x133, DIGB_12 = 0x133;

static int terc4_decode(uint16_t c) { for (int i = 0; i < 16; i++) if (TERC4[i] == c) return i; return -1; }
static int ctrl_decode(uint16_t c) { for (int i = 0; i < 4; i++) if (CTRL[i] == c) return i; return -1; }

static int tmds_video_decode(uint16_t q)
{
    uint16_t d = (q & 0x200) ? (q ^ 0xFF) : q;
    int out = d & 1;
    for (int i = 1; i < 8; i++) {
        int b = ((d >> i) ^ (d >> (i - 1))) & 1;
        if (!(d & 0x100)) b ^= 1;
        out |= b << i;
    }
    return out;
}

static int ones10(uint16_t c) { int n = 0; for (int i = 0; i < 10; i++) n += (c >> i) & 1; return n; }

// ---- BCH ECC, G(x) = 1 + x^6 + x^7 + x^8, bits in transmission order -------------
static uint8_t ecc(const std::vector<int> &bits)
{
    uint8_t e = 0;
    for (int b : bits) {
        int fb = (e ^ b) & 1;
        e >>= 1;
        if (fb) e ^= 0x83;
    }
    return e;
}

// ---- audio out ------------------------------------------------------------------
static uint64_t samples_out;
static void hdmi_on_sample(int32_t l, int32_t r);   // defined by the testbench

static int expected_cs(int n, int right)
{
    return n == 2 || (n == 20 && !right) || (n == 21 && right) || n == 25 || n == 32 || n == 33 || n == 35;
}

// ---- sink state -----------------------------------------------------------------
enum Period { CONTROL, VIDEO_GUARD, VIDEO, ISLAND_GUARD, ISLAND, TRAIL_GUARD };
static Period period = CONTROL;
static int control_run;                  // control characters since the last data period
static int preamble_kind, preamble_run;  // CTL0..3 of the latest control characters, and how many
static int guard_left, packet_clk, packets_in_island;
static bool synced;                      // seen the first preamble
static int disparity[3];

// packet being received
static std::vector<int> hdr_bits, sub_bits[4];
static int island_first;

// sync timing
static int hs = 1, vs = 1;
static uint64_t last_hs_fall, hs_fall_count, lines_since_vs;
static int64_t line_pos = -1;            // clocks since the latest HSYNC fall
static bool timing_locked;
static int video_in_line, video_lines_in_frame, frames_done;

// packets
static uint64_t acr_count, acr_first_clk, acr_last_clk;
static int avi_in_frame, aif_in_frame, frame_checks;
static uint32_t cs_l[6], cs_r[6];        // channel status bits 0..191 of the current block
static int block_pos = -1;               // frame in the 192-frame block, -1 before the first B

static void check_packet()
{
    std::vector<int> h(hdr_bits.begin(), hdr_bits.begin() + 24);
    uint8_t he = 0;
    for (int i = 0; i < 8; i++) he |= hdr_bits[24 + i] << i;
    if (ecc(h) != he) FAIL("header ECC");
    uint8_t hb[3] = {0, 0, 0};
    for (int i = 0; i < 24; i++) hb[i / 8] |= h[i] << (i % 8);
    uint8_t sb[4][7];
    for (int s = 0; s < 4; s++) {
        std::vector<int> d(sub_bits[s].begin(), sub_bits[s].begin() + 56);
        uint8_t se = 0;
        for (int i = 0; i < 8; i++) se |= sub_bits[s][56 + i] << i;
        if (ecc(d) != se) FAIL("subpacket %d ECC", s);
        memset(sb[s], 0, 7);
        for (int i = 0; i < 56; i++) sb[s][i / 8] |= d[i] << (i % 8);
    }
    switch (hb[0]) {
    case 0x00:
        if (hb[1] || hb[2]) FAIL("null packet header not zero");
        for (int s = 0; s < 4; s++) for (int i = 0; i < 7; i++) if (sb[s][i]) FAIL("null packet body not zero");
        break;
    case 0x01: {
        for (int s = 0; s < 4; s++) {
            uint32_t cts = (sb[s][1] & 0xF) << 16 | sb[s][2] << 8 | sb[s][3];
            uint32_t n = (sb[s][4] & 0xF) << 16 | sb[s][5] << 8 | sb[s][6];
            if (sb[s][0] || cts != 25200 || n != 6144) FAIL("ACR subpacket %d: CTS %u N %u", s, cts, n);
        }
        if (acr_count == 0) acr_first_clk = clk_no;
        acr_last_clk = clk_no;
        acr_count++;
        break;
    }
    case 0x02: {
        int layout = (hb[1] >> 4) & 1, present = hb[1] & 0xF, flat = hb[2] & 0xF, b = hb[2] >> 4;
        if (layout || flat) FAIL("audio packet layout %d flat %x", layout, flat);
        if (present != 1 && present != 3 && present != 7 && present != 15) FAIL("audio present bits %x", present);
        for (int s = 0; s < 4; s++) {
            if (!(present >> s & 1)) {
                if (b >> s & 1) FAIL("B set on an absent subpacket");
                continue;
            }
            int32_t l = sb[s][0] | sb[s][1] << 8 | sb[s][2] << 16;
            int32_t r = sb[s][3] | sb[s][4] << 8 | sb[s][5] << 16;
            l = (l << 8) >> 8; r = (r << 8) >> 8;
            int vl = sb[s][6] & 1, ul = sb[s][6] >> 1 & 1, cl = sb[s][6] >> 2 & 1, pl = sb[s][6] >> 3 & 1;
            int vr = sb[s][6] >> 4 & 1, ur = sb[s][6] >> 5 & 1, cr = sb[s][6] >> 6 & 1, pr = sb[s][6] >> 7 & 1;
            if (vl || ul || vr || ur) FAIL("V or U bit set");
            int par_l = vl ^ ul ^ cl ^ pl, par_r = vr ^ ur ^ cr ^ pr;
            for (int i = 0; i < 24; i++) { par_l ^= (l >> i) & 1; par_r ^= (r >> i) & 1; }
            if (par_l || par_r) FAIL("audio parity");
            if (b >> s & 1) {
                if (block_pos >= 0 && block_pos != 192) FAIL("B flag after %d frames", block_pos);
                if (block_pos == 192) {
                    for (int n = 0; n < 192; n++)
                        if ((int)(cs_l[n / 32] >> (n % 32) & 1) != expected_cs(n, 0) ||
                            (int)(cs_r[n / 32] >> (n % 32) & 1) != expected_cs(n, 1)) {
                            FAIL("channel status bit %d", n);
                            break;
                        }
                }
                block_pos = 0;
                memset(cs_l, 0, sizeof cs_l); memset(cs_r, 0, sizeof cs_r);
            } else if (block_pos == 192) {
                FAIL("no B flag after 192 frames");
            }
            if (block_pos >= 0 && block_pos < 192) {
                cs_l[block_pos / 32] |= (uint32_t)cl << (block_pos % 32);
                cs_r[block_pos / 32] |= (uint32_t)cr << (block_pos % 32);
                block_pos++;
            }
            hdmi_on_sample(l, r);
            samples_out++;
        }
        break;
    }
    case 0x82:
    case 0x84: {
        int len = hb[2], sum = hb[0] + hb[1] + hb[2];
        uint8_t pb[28];
        for (int s = 0; s < 4; s++) for (int i = 0; i < 7; i++) pb[s * 7 + i] = sb[s][i];
        for (int i = 0; i <= len; i++) sum += pb[i];
        if (sum & 0xFF) FAIL("InfoFrame %02x checksum", hb[0]);
        for (int i = len + 1; i < 28; i++) if (pb[i]) FAIL("InfoFrame %02x byte %d past its length", hb[0], i);
        if (hb[0] == 0x82) {
            if (hb[1] != 2 || len != 13) FAIL("AVI version %d length %d", hb[1], len);
            if (pb[1] >> 5 & 3) FAIL("AVI not RGB");
            if (pb[4] != 1) FAIL("AVI VIC %d", pb[4]);
            if ((pb[2] >> 4 & 3) != 1) FAIL("AVI aspect not 4:3");
            avi_in_frame++;
        } else {
            if (hb[1] != 1 || len != 10) FAIL("Audio InfoFrame version %d length %d", hb[1], len);
            if ((pb[1] & 7) != 1) FAIL("Audio InfoFrame channel count %d", (pb[1] & 7) + 1);
            if (pb[4] != 0) FAIL("Audio InfoFrame speakers %02x", pb[4]);
            aif_in_frame++;
        }
        break;
    }
    default:
        FAIL("unexpected packet type %02x", hb[0]);
    }
}

static void sync_edges(int new_hs, int new_vs)
{
    if (!synced) { hs = new_hs; vs = new_vs; return; }   // like a sink: nothing counts until locked
    if (hs && !new_hs) {                       // HSYNC falls
        if (timing_locked && clk_no - last_hs_fall != 800) FAIL("line length %llu", (unsigned long long)(clk_no - last_hs_fall));
        last_hs_fall = clk_no;
        hs_fall_count++;
        line_pos = 0;
        lines_since_vs++;
        if (hs_fall_count > 2) timing_locked = true;
    }
    if (!hs && new_hs && timing_locked && clk_no - last_hs_fall != 96) FAIL("HSYNC width %llu", (unsigned long long)(clk_no - last_hs_fall));
    if (vs && !new_vs) {                       // VSYNC falls: with HSYNC
        if (clk_no != last_hs_fall) FAIL("VSYNC doesn't start with HSYNC");
        if (frames_done > 0 && lines_since_vs != 525) FAIL("frame of %llu lines", (unsigned long long)lines_since_vs);
        if (frames_done > 0) {
            if (video_lines_in_frame != 480) FAIL("%d video lines in a frame", video_lines_in_frame);
            if (avi_in_frame < 1 || aif_in_frame < 1) FAIL("frame without AVI (%d) or Audio InfoFrame (%d)", avi_in_frame, aif_in_frame);
            frame_checks++;
        }
        avi_in_frame = aif_in_frame = 0;
        video_lines_in_frame = 0;
        lines_since_vs = 0;
        frames_done++;
    }
    if (!vs && new_vs && timing_locked) {
        if (clk_no != last_hs_fall || lines_since_vs != 2) FAIL("VSYNC width %llu lines", (unsigned long long)lines_since_vs);
    }
    hs = new_hs;
    vs = new_vs;
}

static void sink(uint16_t c0, uint16_t c1, uint16_t c2)
{
    uint16_t ch[3] = {c0, c1, c2};
    if (line_pos >= 0) line_pos++;
    int k0 = ctrl_decode(c0), k1 = ctrl_decode(c1), k2 = ctrl_decode(c2);
    bool all_ctrl = k0 >= 0 && k1 >= 0 && k2 >= 0;

    switch (period) {
    case CONTROL:
        if (all_ctrl) {
            sync_edges(k0 & 1, k0 >> 1 & 1);
            int kind = k1 | k2 << 2;           // CTL0, CTL1, CTL2, CTL3
            if (kind == preamble_kind) preamble_run++;
            else { preamble_kind = kind; preamble_run = 1; }
            if (kind != 0 && kind != 1 && kind != 5) FAIL("illegal CTL value %x", kind);
            control_run++;
            break;
        }
        if (c0 == VGB_02 && c1 == VGB_1 && c2 == VGB_02 && preamble_kind == 1) {
            if (preamble_run != 8) FAIL("video preamble of %d", preamble_run);
            if (control_run < 12) FAIL("control period of %d", control_run);
            period = VIDEO_GUARD; guard_left = 1; synced = true;
            break;
        }
        if (c1 == DIGB_12 && c2 == DIGB_12 && terc4_decode(c0) >= 12 && preamble_kind == 5) {
            int d = terc4_decode(c0);
            sync_edges(d & 1, d >> 1 & 1);
            if (preamble_run != 8) FAIL("island preamble of %d", preamble_run);
            if (control_run < 12) FAIL("control period of %d", control_run);
            period = ISLAND_GUARD; guard_left = 1; synced = true;
            break;
        }
        if (synced) FAIL("unexpected character %03x %03x %03x in a control period", c0, c1, c2);
        break;
    case VIDEO_GUARD:
        if (!(c0 == VGB_02 && c1 == VGB_1 && c2 == VGB_02)) FAIL("video guard band");
        if (--guard_left == 0) { period = VIDEO; video_in_line = 0; for (int i = 0; i < 3; i++) disparity[i] = 0; }
        break;
    case VIDEO:
        if (all_ctrl) {
            if (video_in_line != 640) FAIL("video line of %d pixels", video_in_line);
            video_lines_in_frame++;
            period = CONTROL; control_run = 0; preamble_run = 0; preamble_kind = -1;
            sink(c0, c1, c2);                // this character starts the control period
            line_pos--;                       // (counted twice)
            return;
        }
        if (video_in_line == 0 && timing_locked && frames_done > 0) {
            if (line_pos != 144) FAIL("video starts %lld clocks after HSYNC", (long long)line_pos);
            if (lines_since_vs < 34 || lines_since_vs > 34 + 479) FAIL("video on line %llu after VSYNC", (unsigned long long)lines_since_vs);
        }
        for (int i = 0; i < 3; i++) {
            int v = tmds_video_decode(ch[i]);
            int want = (COLOUR >> (8 * i)) & 0xFF;
            if (v != want) FAIL("pixel channel %d = %02x, want %02x", i, v, want);
            disparity[i] += 2 * ones10(ch[i]) - 10;
            if (disparity[i] > 20 || disparity[i] < -20) FAIL("channel %d disparity %d", i, disparity[i]);
        }
        video_in_line++;
        break;
    case ISLAND_GUARD:
    case TRAIL_GUARD: {
        int d = terc4_decode(c0);
        if (!(c1 == DIGB_12 && c2 == DIGB_12 && d >= 12)) FAIL("island guard band");
        else sync_edges(d & 1, d >> 1 & 1);
        if (--guard_left == 0) {
            if (period == ISLAND_GUARD) {
                period = ISLAND; packet_clk = 0; packets_in_island = 0; island_first = 1;
            } else {
                period = CONTROL; control_run = 0; preamble_run = 0; preamble_kind = -1;
            }
        }
        break;
    }
    case ISLAND: {
        if (packet_clk == 0 && c1 == DIGB_12 && c2 == DIGB_12) {   // trailing guard band
            if (packets_in_island < 1 || packets_in_island > 18) FAIL("island of %d packets", packets_in_island);
            period = TRAIL_GUARD; guard_left = 2;
            sink(c0, c1, c2);
            line_pos--;
            return;
        }
        int d0 = terc4_decode(c0), d1 = terc4_decode(c1), d2 = terc4_decode(c2);
        if (d0 < 0 || d1 < 0 || d2 < 0) { FAIL("non-TERC4 character in an island"); break; }
        sync_edges(d0 & 1, d0 >> 1 & 1);
        int d3 = d0 >> 3 & 1;
        if (d3 != (island_first ? 0 : 1)) FAIL("channel 0 bit 3 = %d (packet clock %d)", d3, packet_clk);
        island_first = 0;
        if (packet_clk == 0) {
            hdr_bits.clear();
            for (int s = 0; s < 4; s++) sub_bits[s].clear();
        }
        hdr_bits.push_back(d0 >> 2 & 1);
        for (int s = 0; s < 4; s++) {
            sub_bits[s].push_back(d1 >> s & 1);
            sub_bits[s].push_back(d2 >> s & 1);
        }
        if (++packet_clk == 32) {
            check_packet();
            packet_clk = 0;
            packets_in_island++;
        }
        break;
    }
    }
}

#endif
