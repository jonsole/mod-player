// The spectrum analyser on the HDMI picture (issue #24, ADR-0014): 24 bars
// of the mix's spectrum in rows 12-19 of the text console (y 288..479).
// The analysis matches the C model (host/spectrum.c, which documents it)
// bar for bar.
//
// System clock: the Resampler's 48 kHz samples go into a ring of 1024. Once
// a video frame (while enabled): load the last 1024, windowed, into the FFT
// RAM in bit-reversed order (1027 clocks); 10 FFT stages of 512 butterflies,
// two clocks each (1028 clocks a stage); then each bar's loudest bin, its
// height, its fall and its peak (one clock a bin). About 12 000 clocks
// (0.24 ms) in all.
//
// The FFT RAM's ports are registered: an address set at the end of clock k
// is used in clock k+1. Butterflies read on odd clocks (issued on even
// ones) and write on even clocks, so a port is never asked for both.
//
// Pixel clock: the bars shown are copied at the start of the vertical
// blanking, the same moment the next analysis is started; that analysis
// changes the bars 0.24 ms later, long before the next copy, so the copy
// never catches them half updated. Each bar is a stack of 25 LEDs like a
// vacuum fluorescent display's (ADR-0016): 23 x 5 pixels, 26 apart across
// from x = 8 and 7 apart up from y = 479. LED k (from 0 at the bottom) is lit
// when the bar's height is at least 7 (k + 1); the peak is the one LED with
// 7 k <= peak < 7 k + 7, from k = 1, brighter than the bar; unlit LEDs glow faintly, as a VFD's
// unlit segments do. Like the console, the colour for pixel
// (pixel_x, pixel_y) is ready 5 clocks later: on = this pixel is the
// analyser's.
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
`default_nettype none
module spectrum #(
    parameter WINDOW = "spectrum_window.hex",
    parameter SINE   = "spectrum_sine.hex",
    parameter BARS   = "spectrum_bars.hex"
) (
    // system clock
    input wire                clk,
    input wire                rst,
    input wire                enable,         // the console's control register
    input wire                in_valid,       // the Resampler's 48 kHz samples
    input wire signed [23:0]  in_l,
    input wire signed [23:0]  in_r,
    // pixel clock
    input wire                clk_pixel,
    input wire                rst_pixel,
    input wire [9:0]          pixel_x,
    input wire [9:0]          pixel_y,
    output reg                on,
    output reg [23:0]         rgb,
    // for testbenches (leave unconnected): an analysis starts / has updated
    // the bars (system clock); the bars and peaks; the display took its copy
    // (pixel clock)
    output wire               dbg_start,
    output reg                dbg_done,
    output wire [191:0]       dbg_bars,
    output wire [191:0]       dbg_peaks,
    output wire               dbg_copy
);
    localparam FLOOR = 9'd16, HEIGHT = 9'd180, FALL = 8'd4, HOLD = 5'd30;

    // ---- the tables (tools/spectrum_tables.py) ----------------------------------------
    reg [15:0] window [0:1023];
    (* rom_style = "distributed" *) reg [17:0] sine [0:256];
    (* rom_style = "distributed" *) reg [19:0] bar_bins [0:23];    // first bin << 10 | last bin
    initial begin
        $readmemh(WINDOW, window);
        $readmemh(SINE, sine);
        $readmemh(BARS, bar_bins);
    end

    // ---- the ring of samples ---------------------------------------------------------
    wire signed [24:0] mix = {in_l[23], in_l} + {in_r[23], in_r};
    wire signed [17:0] sample = {mix[24], mix[24:8]};          // (left + right) >> 8, sign extended
    reg  [17:0] ring [0:1023];
    reg  [9:0]  wp;                                            // the next slot: the oldest sample
    always @(posedge clk) begin
        if (rst)
            wp <= 10'd0;
        else if (in_valid) begin
            ring[wp] <= sample;
            wp <= wp + 1'b1;
        end
    end

    // ---- frame events from the pixel clock --------------------------------------------
    reg  [2:0] frame_sync;
    reg        frame_t;
    wire       new_frame = frame_sync[2] != frame_sync[1];

    // ---- the analysis ----------------------------------------------------------------
    localparam S_IDLE = 2'd0, S_LOAD = 2'd1, S_FFT = 2'd2, S_BARS = 2'd3;
    reg  [1:0]  state;
    wire        start = state == S_IDLE && new_frame && enable;
    assign dbg_start = start;
    reg  [9:0]  base;                                          // the oldest sample
    reg  [10:0] n;                                             // LOAD: sample; FFT: clock in stage
    reg  [3:0]  stage;

    // FFT RAM: re and im, 1024 x 18 each, two ports
    reg  [17:0] fre [0:1023];
    reg  [17:0] fim [0:1023];
    reg  [9:0]  addr_a, addr_b;
    reg         we_a, we_b;
    reg  [17:0] wre_a, wim_a, wre_b, wim_b;
    reg  [17:0] qre_a, qim_a, qre_b, qim_b;
    always @(posedge clk) begin
        if (we_a) begin
            fre[addr_a] <= wre_a;
            fim[addr_a] <= wim_a;
        end
        qre_a <= fre[addr_a];
        qim_a <= fim[addr_a];
    end
    always @(posedge clk) begin
        if (we_b) begin
            fre[addr_b] <= wre_b;
            fim[addr_b] <= wim_b;
        end
        qre_b <= fre[addr_b];
        qim_b <= fim[addr_b];
    end

    function [9:0] bit_reverse;
        input [9:0] v;
        integer k;
        for (k = 0; k < 10; k = k + 1)
            bit_reverse[k] = v[9 - k];
    endfunction

    // LOAD pipeline: ring and window read (1), product (2), write (3)
    reg  [17:0] ring_q;
    reg  [15:0] win_q;
    reg  [9:0]  ld_n1, ld_n2;
    reg         ld_v1, ld_v2;
    reg  signed [34:0] ld_prod;
    always @(posedge clk) begin
        ring_q <= ring[base + n[9:0]];
        win_q <= window[n[9:0]];
    end

    // FFT pipeline, for a butterfly issued on even clock c: addresses used
    // (c+1), data (c+2), products (c+3), sums (c+4), results (c+5), written
    // (c+6, even)
    wire [8:0]  j = n[9:1];                                    // butterfly in the stage
    wire [9:0]  half = 10'd1 << stage;
    wire [9:0]  pos = {1'b0, j} & (half - 1'b1);
    wire [9:0]  bf_a = (({1'b0, j} >> stage) << (stage + 1)) | pos;
    wire [9:0]  bf_b = bf_a + half;
    wire [8:0]  tw_k = pos << (4'd9 - stage);                  // 0..511
    wire [17:0] sin_k = tw_k <= 9'd256 ? sine[tw_k[8:0]] : sine[9'd512 - tw_k];
    wire [17:0] cos_k = tw_k <= 9'd256 ? sine[9'd256 - tw_k] : 18'd0 - sine[tw_k - 9'd256];
    wire        issue = state == S_FFT && !n[0] && n < 11'd1024;
    reg         v1, v2, v3, v4, v5;
    reg  [9:0]  a1, b1, a2, b2, a3, b3, a4, b4, a5, b5;
    reg  signed [17:0] wr1, wi1, wr2, wi2;                     // W = cos - i sin
    reg  signed [17:0] ar3, ai3, ar4, ai4;
    reg  signed [35:0] p_rr, p_ii, p_ri, p_ir;
    reg  signed [17:0] tr4, ti4;
    reg  [17:0] ra5, ia5, rb5, ib5;
    wire signed [36:0] sum_r = {p_rr[35], p_rr} - {p_ii[35], p_ii} + 37'sd65536;
    wire signed [36:0] sum_i = {p_ri[35], p_ri} + {p_ir[35], p_ir} + 37'sd65536;
    wire signed [18:0] a_r_plus  = ar4 + tr4 + 19'sd1, a_r_minus = ar4 - tr4 + 19'sd1;
    wire signed [18:0] a_i_plus  = ai4 + ti4 + 19'sd1, a_i_minus = ai4 - ti4 + 19'sd1;

    // BARS pipeline, for a bin issued on clock c: address used (c+1), data
    // (c+2), squares (c+3), the power's log (c+4), the bar (c+4's end)
    reg  [5:0]  bar;
    reg  [9:0]  bin;
    wire [9:0]  bin_hi = bar_bins[bar][9:0];
    reg         bv1, bv2, bv3, bv4, bl1, bl2, bl3, bl4;        // valid; last bin of its bar
    reg  [5:0]  bb1, bb2, bb3, bb4;
    reg  [35:0] sq_r, sq_i;
    reg  [8:0]  lvl4;
    reg  [8:0]  acc;
    reg  [7:0]  bar_h [0:23];
    reg  [7:0]  peak_h [0:23];
    reg  [4:0]  hold [0:23];
    reg         bars_issue;

    function [8:0] log8;                                       // 8 x msb + the next 3 bits
        input [35:0] p;
        integer k;
        reg [5:0] m;
        reg       found;
        reg [35:0] sh;
        begin
            m = 6'd0;
            found = 1'b0;
            for (k = 35; k >= 0; k = k - 1)
                if (!found && p[k]) begin
                    m = k;
                    found = 1'b1;
                end
            sh = m >= 6'd3 ? p >> (m - 6'd3) : p << (6'd3 - m);
            log8 = found ? {m, sh[2:0]} : 9'd0;
        end
    endfunction

    wire [8:0] level = lvl4 > acc ? lvl4 : acc;                // the bar's loudest so far
    wire [8:0] h_raw = level <= FLOOR ? 9'd0 : level - FLOOR;
    wire [7:0] h_new = h_raw > HEIGHT ? HEIGHT[7:0] : h_raw[7:0];
    wire [7:0] fallen = bar_h[bb4] > FALL ? bar_h[bb4] - FALL : 8'd0;
    wire [7:0] shown = h_new > fallen ? h_new : fallen;

    integer i;
    always @(posedge clk) begin
        we_a <= 1'b0;
        we_b <= 1'b0;
        dbg_done <= 1'b0;
        frame_sync <= {frame_sync[1:0], frame_t};
        if (rst) begin
            state <= S_IDLE;
            n <= 11'd0;
            stage <= 4'd0;
            ld_v1 <= 1'b0; ld_v2 <= 1'b0;
            v1 <= 1'b0; v2 <= 1'b0; v3 <= 1'b0; v4 <= 1'b0; v5 <= 1'b0;
            bv1 <= 1'b0; bv2 <= 1'b0; bv3 <= 1'b0; bv4 <= 1'b0;
            bars_issue <= 1'b0;
            for (i = 0; i < 24; i = i + 1) begin
                bar_h[i] <= 8'd0;
                peak_h[i] <= 8'd0;
                hold[i] <= 5'd0;
            end
        end else begin
            // ---- LOAD: windowed samples, bit-reversed ----
            ld_v1 <= state == S_LOAD && n < 11'd1024;
            ld_n1 <= n[9:0];
            ld_v2 <= ld_v1;
            ld_n2 <= ld_n1;
            ld_prod <= $signed(ring_q) * $signed({1'b0, win_q}) + 35'sd16384;
            if (ld_v2) begin
                we_a <= 1'b1;
                addr_a <= bit_reverse(ld_n2);
                wre_a <= ld_prod[32:15];
                wim_a <= 18'd0;
            end

            // ---- FFT ----
            v1 <= issue;
            a1 <= bf_a; b1 <= bf_b;
            wr1 <= cos_k;
            wi1 <= 18'd0 - sin_k;
            v2 <= v1;                                          // c+1: the RAM reads
            a2 <= a1; b2 <= b1;
            wr2 <= wr1; wi2 <= wi1;
            v3 <= v2;                                          // c+2: data in, products out
            a3 <= a2; b3 <= b2;
            ar3 <= qre_a; ai3 <= qim_a;
            p_rr <= $signed(qre_b) * wr2;
            p_ii <= $signed(qim_b) * wi2;
            p_ri <= $signed(qre_b) * wi2;
            p_ir <= $signed(qim_b) * wr2;
            v4 <= v3;                                          // c+3: t = b x W
            a4 <= a3; b4 <= b3;
            ar4 <= ar3; ai4 <= ai3;
            tr4 <= sum_r[34:17];
            ti4 <= sum_i[34:17];
            v5 <= v4;                                          // c+4: a + t, a - t, halved
            a5 <= a4; b5 <= b4;
            ra5 <= a_r_plus[18:1];
            ia5 <= a_i_plus[18:1];
            rb5 <= a_r_minus[18:1];
            ib5 <= a_i_minus[18:1];
            if (issue) begin                                   // even clock: read a and b
                addr_a <= bf_a;
                addr_b <= bf_b;
            end
            if (v5) begin                                      // c+5, odd: write a' and b' (in c+6)
                we_a <= 1'b1;
                we_b <= 1'b1;
                addr_a <= a5;
                addr_b <= b5;
                wre_a <= ra5;
                wim_a <= ia5;
                wre_b <= rb5;
                wim_b <= ib5;
            end

            // ---- BARS: each bar's loudest bin ----
            if (bars_issue)
                addr_a <= bin;
            bv1 <= bars_issue;
            bl1 <= bin == bin_hi;
            bb1 <= bar;
            bv2 <= bv1; bl2 <= bl1; bb2 <= bb1;
            sq_r <= $signed(qre_a) * $signed(qre_a);
            sq_i <= $signed(qim_a) * $signed(qim_a);
            bv3 <= bv2; bl3 <= bl2; bb3 <= bb2;
            bv4 <= bv3; bl4 <= bl3; bb4 <= bb3;
            lvl4 <= log8(sq_r + sq_i);
            if (bv4) begin
                if (bl4) begin                                 // the bar's last bin: update it
                    acc <= 9'd0;
                    bar_h[bb4] <= shown;
                    if (shown >= peak_h[bb4]) begin
                        peak_h[bb4] <= shown;
                        hold[bb4] <= HOLD;
                    end else if (hold[bb4] != 5'd0)
                        hold[bb4] <= hold[bb4] - 1'b1;
                    else
                        peak_h[bb4] <= peak_h[bb4] - 1'b1;
                    if (bb4 == 6'd23)
                        dbg_done <= 1'b1;
                end else
                    acc <= level;
            end

            // ---- sequencing ----
            case (state)
                S_IDLE:
                    if (start) begin
                        base <= in_valid ? wp + 1'b1 : wp;    // a sample arriving now is the newest
                        n <= 11'd0;
                        state <= S_LOAD;
                    end
                S_LOAD:
                    if (n == 11'd1026) begin                  // 1024 issued, 2 more to drain
                        n <= 11'd0;
                        stage <= 4'd0;
                        state <= S_FFT;
                    end else
                        n <= n + 1'b1;
                S_FFT:
                    if (n == 11'd1027) begin                  // 512 butterflies; the last is written
                                                              // as the next stage's first is issued
                        n <= 11'd0;
                        if (stage == 4'd9) begin
                            state <= S_BARS;
                            bar <= 6'd0;
                            bin <= bar_bins[0][19:10];
                            bars_issue <= 1'b1;
                            acc <= 9'd0;
                        end else
                            stage <= stage + 1'b1;
                    end else
                        n <= n + 1'b1;
                S_BARS:
                    if (bars_issue) begin
                        if (bin == bin_hi) begin
                            if (bar == 6'd23)
                                bars_issue <= 1'b0;
                            else begin
                                bar <= bar + 1'b1;
                                bin <= bar_bins[bar + 1'b1][19:10];
                            end
                        end else
                            bin <= bin + 1'b1;
                    end else if (!bv1 && !bv2 && !bv3 && !bv4)
                        state <= S_IDLE;
            endcase
        end
    end

    genvar g;
    generate
        for (g = 0; g < 24; g = g + 1) begin : dbg
            assign dbg_bars[8 * g + 7 : 8 * g] = bar_h[g];
            assign dbg_peaks[8 * g + 7 : 8 * g] = peak_h[g];
        end
    endgenerate

    // ---- pixel clock: the bars shown ----------------------------------------------------
    reg [7:0] show_h [0:23];
    reg [7:0] show_p [0:23];
    reg [2:0] en_sync;
    wire      copy = pixel_x == 10'd0 && pixel_y == 10'd480;
    assign dbg_copy = copy;
    always @(posedge clk_pixel) begin
        if (rst_pixel) begin
            frame_t <= 1'b0;
            en_sync <= 3'd0;
        end else begin
            en_sync <= {en_sync[1:0], enable};
            if (copy) begin
                frame_t <= !frame_t;                          // start the next analysis
                for (i = 0; i < 24; i = i + 1) begin
                    show_h[i] <= bar_h[i];
                    show_p[i] <= peak_h[i];
                end
            end
        end
    end

    // stage A: which bar and how far across it; which LED and how far up it
    wire [9:0]  bx = pixel_x - 10'd8;
    wire [20:0] bar_m = bx * 11'd1261;                         // bx / 26, exact for bx < 624
    wire [4:0]  bar_n = bar_m[19:15];
    wire [9:0]  bar_x26 = {bar_n, 4'b0000} + {bar_n, 3'b000} + {bar_n, 1'b0};   // 26 x bar
    wire [7:0]  pos_n = 8'd223 - pixel_y[7:0];                 // 479 - y, for y in 288..479
    wire [16:0] led_m = pos_n * 9'd293;                        // pos / 7, exact for pos < 256
    wire [4:0]  led_n = led_m[15:11];
    wire [7:0]  led_x7 = {led_n, 3'b000} - led_n;              // 7 x LED
    reg  [4:0]  bar_a, led_a, led_b;
    reg  [7:0]  led7_a, led7_b;                                // 7 x LED: its bottom
    reg         in_a, in_b;
    always @(posedge clk_pixel) begin
        bar_a <= bar_n;
        led_a <= led_n;
        led7_a <= led_x7;
        in_a <= en_sync[1] && pixel_x >= 10'd8 && pixel_x < 10'd632 && pixel_y >= 10'd288 && pixel_y < 10'd480 &&
                bx - bar_x26 < 10'd23 &&                       // across the LED, not the gap
                pos_n - led_x7 < 8'd5 && led_n < 5'd25;        // up the LED, not the gap; 25 LEDs
    end

    // stage B: the bar's height and peak
    reg [7:0] h_b, p_b;
    always @(posedge clk_pixel) begin
        h_b <= show_h[bar_a];
        p_b <= show_p[bar_a];
        led_b <= led_a;
        led7_b <= led7_a;
        in_b <= in_a;
    end

    // stage C: lit as part of the bar, or as the peak (always above the bar's top LED)
    wire [8:0] top7 = {1'b0, led7_b} + 9'd7;                   // 7 (k + 1)
    reg bar_c, peak_c, ghost_c, bar_d, peak_d, ghost_d;
    always @(posedge clk_pixel) begin
        bar_c <= in_b && {1'b0, h_b} >= top7;
        peak_c <= in_b && led_b != 5'd0 && p_b >= led7_b && {1'b0, p_b} < top7;
        ghost_c <= in_b;
        bar_d <= bar_c;
        peak_d <= peak_c;
        ghost_d <= ghost_c;
    end

    // stage E: the peak bright white, the bar a little dimmer, unlit a faint grey
    always @(posedge clk_pixel) begin
        on <= ghost_d;
        rgb <= peak_d ? 24'hF4F4F4 : bar_d ? 24'hB0B0B0 : 24'h121212;
    end
endmodule
`default_nettype wire
