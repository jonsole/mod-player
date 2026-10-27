// Four oscilloscope traces on the HDMI picture, one per Voice (issue #23,
// ADR-0013), drawn from Paula's own per-Voice output (sample x volume), so
// they cost the CPU nothing. They sit in a 2 x 2 grid in rows 4-11 of the
// text console (y 96..287): Voices 1 and 2 on top, 3 and 4 below, each 316
// pixels wide and 96 tall, the left ones from x = 2 and the right ones from
// x = 322 (a 4-pixel gap between); the console's text shows through
// wherever there is no trace.
//
// Capture (system clock, scope_capture below): once a frame, each Voice
// waits for its output to cross zero going up (as a scope triggers; or
// gives up after 10 ms, so notes down to about 99 Hz trigger), then takes
// 316 points, one every 256 colour clocks (22.8 ms in all). A Voice's
// output is held between Paula's sample changes, which would draw as
// steps; so the point at a time between two changes is on the straight
// line joining them. The capture finds the changes (the output's value
// changes at least 64 frames after the last; or, if it stays the same for 64
// frames while 0, or 1100 otherwise, it takes one anyway), and when the next
// change arrives one engine, shared by the four Voices, divides to get the
// slope, then writes the points that fall between the two, one a clock. Each point is the Voice's output * 3 / 32 in 1/16
// pixels, so >> 4 gives pixels: -48..47 (a full-scale Voice fills its
// scope; 7 bits stored). The arithmetic is integer and is defined by the
// model in hw/sim/console_tb.cpp (the same, line for line).
//
// Persistence: each Voice keeps its last captures in a ring of 4 slots.
// The screen shows the newest at full brightness and the two before it at
// three quarters and half, like a scope's phosphor fading; the fourth slot is
// the one being captured into. A capture fills the slot after the newest;
// when all four Voices are done that slot becomes the newest, and the
// screen takes it at the next frame. A new capture starts only once the
// screen has taken the last one, so the slot being written is never on
// screen. A capture takes a few video frames, so the scopes update every
// two or three frames.
//
// Display (pixel clock): point i of a capture is column i of its scope, its
// value pixels above the scope's centre line. The trace is the straight
// line through the points, so a column is lit from its midpoint with the
// column before to its midpoint with the column after, taking in its own
// point (in half pixels): a slope of one pixel a column is a single pixel
// in each column, a steep one a run of pixels, a peak or a flat run just
// its point. A dim centre line runs through each scope. Like the console, the colour for pixel
// (pixel_x, pixel_y) is ready 5 clocks later: on = this pixel is a scope's.
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
`default_nettype none

// One Voice's capture. It records from the moment it starts, the Voice's
// output a column every 256 frames, into its slot as a ring of 316 points;
// it finds the Voice's sample changes ("knots") and asks the shared engine
// (scope_engine) to draw the segment between each pair. Once 158 columns are
// in, the next rising zero crossing is the trigger (or, after TIMEOUT more
// frames, the frame then); the capture goes on for 158 columns from the one
// at or after it, so the trigger is in the middle of the window: columns
// first_col .. first_col + 315 of the ring, column 158 the trigger's. Frames
// come with audio_valid, one per colour clock.
module scope_capture (
    input wire               clk,
    input wire               rst,
    input wire               audio_valid,
    input wire signed [15:0] v,             // the Voice's output this frame
    input wire               start,         // begin recording
    input wire               clear,         // back to idle
    output reg [1:0]         phase,         // 0 idle, 1 before the trigger, 2 after it, 3 done
    // the first point
    output reg               wen,           // a point to write (valid the clock after)
    output reg [8:0]         widx,
    output reg [6:0]         wdat,          // signed, -48..47
    // a segment for the engine: held until the engine takes it
    output reg               req,
    output reg [10:0]        s_gap,         // frames from the last knot to this one
    output reg [10:0]        s_off,         // the next column's time after the last knot
    output reg signed [10:0] s_q0,          // the last knot's value, 1/16 pixel
    output reg               s_neg,         // this knot is below it
    output reg [10:0]        s_dq,          // by this much
    input wire               take,          // the engine has copied the segment
    output wire [9:0]        j_out,         // columns written (counting on past the ring's end)
    output reg [9:0]         stop,          // the last column to write; 1023 until the trigger
    output reg [8:0]         first_col,     // the window's first column in the ring
    input wire               done,          // the engine has drawn the segment
    input wire [10:0]        off_new,
    input wire [9:0]         j_new,
    input wire               finished       // and it was the last column
);
    localparam [16:0] PRE = 17'd40448;      // 158 columns x 256 frames
    localparam [16:0] TIMEOUT = 17'd36000;
    // A change is a knot when it is at least 64 frames (256 clocks: longer
    // than the engine takes to get to a Voice, four to a turn) after the last
    // one; a Voice that doesn't change takes a knot anyway after 64 frames if
    // its output is 0, 1100 otherwise.
    localparam [10:0] IDLE_ZERO = 11'd63, IDLE_HELD = 11'd1099, MIN_AGE = 11'd63;

    reg  [15:0] prev_v;
    reg  [16:0] t;                          // the frame's time: frames since the first point
    reg         begun;                      // the first point is written
    reg  [10:0] age;                        // frames since the last knot, less one
    reg  [10:0] off;
    reg         pending;                    // a change waiting for MIN_AGE
    reg  signed [10:0] q0;
    reg  [9:0]  j;                          // columns written
    assign j_out = j;

    // Q(v) = (3 v) >> 5
    wire signed [17:0] v3 = {{2{v[15]}}, v} + {v[15], v, 1'b0};
    wire signed [10:0] qv = v3[15:5];
    wire        trigger = prev_v[15] && !v[15];
    wire        chg = v != prev_v;
    wire        stalled = age == (v == 16'd0 ? IDLE_ZERO : IDLE_HELD);
    wire        knot = (age >= MIN_AGE && (chg || pending)) || stalled;
    wire signed [11:0] dq = {qv[10], qv} - {q0[10], q0};
    wire [11:0] dq_neg = 12'd0 - dq;
    wire [10:0] dq_abs = dq[11] ? dq_neg[10:0] : dq[10:0];
    wire [16:0] t_up = t + 17'd255;
    wire [9:0]  trig_col = t_up[16:8];      // the column at or after this frame: ceil(t / 256)

    always @(posedge clk) begin
        wen <= 1'b0;
        if (rst) begin
            phase <= 2'd0;
            req <= 1'b0;
            prev_v <= 16'd0;
            t <= 17'd0;
            begun <= 1'b0;
            age <= 11'd0;
            off <= 11'd0;
            pending <= 1'b0;
            q0 <= 11'sd0;
            j <= 10'd0;
            stop <= 10'h3FF;
            first_col <= 9'd0;
            widx <= 9'd0;
            wdat <= 7'd0;
        end else begin
            if (audio_valid)
                prev_v <= v;
            if (take)
                req <= 1'b0;

            if (start) begin
                phase <= 2'd1;
                begun <= 1'b0;
                stop <= 10'h3FF;
            end else if (clear)
                phase <= 2'd0;
            else if (audio_valid && (phase == 2'd1 || phase == 2'd2)) begin
                if (!begun) begin                             // the first point, now
                    begun <= 1'b1;
                    wen <= 1'b1;
                    widx <= 9'd0;
                    wdat <= qv[10:4];
                    j <= 10'd1;
                    q0 <= qv;
                    age <= 11'd0;
                    off <= 11'd256;
                    pending <= 1'b0;
                    t <= 17'd1;
                end else begin
                    t <= t + 1'b1;
                    if (phase == 2'd1 && ((trigger && t >= PRE) || t == PRE + TIMEOUT)) begin
                        phase <= 2'd2;                        // the trigger: 158 columns more
                        stop <= trig_col + 10'd157;
                        first_col <= trig_col[8:0] - 9'd158;
                    end
                    if (knot) begin                           // the next knot: a segment to draw
                        req <= 1'b1;
                        s_gap <= age + 1'b1;
                        s_off <= off;
                        s_q0 <= q0;
                        s_neg <= dq[11];
                        s_dq <= dq_abs;
                        q0 <= qv;
                        age <= 11'd0;
                        pending <= 1'b0;
                    end else begin
                        age <= age + 1'b1;
                        if (chg)
                            pending <= 1'b1;
                    end
                end
            end

            if (done) begin                                   // the engine drew it
                off <= off_new;
                j <= j_new;
                if (finished)
                    phase <= 2'd3;
            end
        end
    end
endmodule

// The interpolator, shared by the four Voices. For the segment between two
// knots: a division for the slope, a multiplication for the first column's
// offset, then a column a clock. One segment takes under 50 clocks and each
// Voice has one at most every 256 clocks, so four Voices never wait long.
module scope_engine (
    input wire         clk,
    input wire         rst,
    input wire [3:0]   req,
    input wire [43:0]  g_gap,               // each Voice's segment, 11 bits each
    input wire [43:0]  g_off,
    input wire [43:0]  g_q0,
    input wire [3:0]   g_neg,
    input wire [43:0]  g_dq,
    input wire [39:0]  g_j,                 // columns written, 10 bits each (counting on past 315)
    input wire [39:0]  g_stop,              // the last column to write, 10 bits each
    output reg [3:0]   take,
    output reg [3:0]   done,
    output reg [10:0]  off_out,
    output reg [9:0]   j_out,
    output reg         finished,
    output reg         wen,
    output reg [8:0]   widx,
    output reg [6:0]   wdat,
    output reg [1:0]   wsel
);
    localparam E_IDLE = 3'd0, E_DIV = 3'd1, E_SET = 3'd2, E_MUL = 3'd3, E_EMIT = 3'd4, E_END = 3'd5;
    reg  [2:0]  es;
    reg  [4:0]  ecnt;
    reg  [1:0]  sel;
    reg  [10:0] s_gap, s_off, c_off;
    reg  signed [10:0] s_q0;
    reg         s_neg;
    reg  [22:0] qq;                         // dividend, then quotient
    reg  [11:0] rem;
    reg  signed [23:0] slope;               // 4096 x (dq / gap), truncated
    reg  signed [35:0] acc, mulsh;
    reg  [10:0] mulb;
    reg  [9:0]  jc, s_stop;
    reg         fin;
    wire [11:0] rem_n = {rem[10:0], qq[22]};
    wire        ge = rem_n >= {1'b0, s_gap};
    wire signed [23:0] slope_n = s_neg ? -$signed({1'b0, qq}) : $signed({1'b0, qq});
    wire signed [35:0] addend = es == E_MUL ? mulsh : {{4{slope[23]}}, slope, 8'b0};
    wire signed [11:0] qt = {s_q0[10], s_q0} + acc[23:12];
    wire [1:0]  pick = req[0] ? 2'd0 : req[1] ? 2'd1 : req[2] ? 2'd2 : 2'd3;

    always @(posedge clk) begin
        wen <= 1'b0;
        take <= 4'd0;
        done <= 4'd0;
        if (rst) begin
            es <= E_IDLE;
            fin <= 1'b0;
        end else begin
            case (es)
            E_IDLE:
                if (req != 4'd0) begin
                    sel <= pick;
                    take[pick] <= 1'b1;
                    s_gap <= g_gap[11 * pick +: 11];
                    s_off <= g_off[11 * pick +: 11];
                    s_q0 <= g_q0[11 * pick +: 11];
                    s_neg <= g_neg[pick];
                    qq <= {g_dq[11 * pick +: 11], 12'd0};
                    rem <= 12'd0;
                    jc <= g_j[10 * pick +: 10];
                    s_stop <= g_stop[10 * pick +: 10];
                    fin <= 1'b0;
                    ecnt <= 5'd0;
                    es <= E_DIV;
                end
            E_DIV: begin                                       // 23 restoring steps
                rem <= ge ? rem_n - {1'b0, s_gap} : rem_n;
                qq <= {qq[21:0], ge};
                ecnt <= ecnt + 1'b1;
                if (ecnt == 5'd22)
                    es <= E_SET;
            end
            E_SET: begin
                slope <= slope_n;
                mulsh <= {{12{slope_n[23]}}, slope_n};
                mulb <= s_off;
                acc <= 36'sd0;
                c_off <= s_off;
                ecnt <= 5'd0;
                es <= E_MUL;
            end
            E_MUL: begin                                       // slope x offset, a bit a clock
                if (mulb[0])
                    acc <= acc + addend;
                mulsh <= {mulsh[34:0], 1'b0};
                mulb <= {1'b0, mulb[10:1]};
                ecnt <= ecnt + 1'b1;
                if (ecnt == 5'd10)
                    es <= E_EMIT;
            end
            E_EMIT: begin
                if (c_off < s_gap) begin                       // a column in this segment
                    wen <= 1'b1;
                    wsel <= sel;
                    widx <= jc >= 10'd316 ? jc[8:0] - 9'd316 : jc[8:0];   // round the ring
                    wdat <= qt[10:4];
                    jc <= jc + 1'b1;
                    acc <= acc + addend;
                    c_off <= c_off + 11'd256;
                    if (jc == s_stop) begin
                        fin <= 1'b1;
                        es <= E_END;
                    end
                end else
                    es <= E_END;
            end
            E_END: begin                                       // tell the Voice
                done[sel] <= 1'b1;
                off_out <= c_off - s_gap;
                j_out <= jc;
                finished <= fin;
                es <= E_IDLE;
            end
            default: es <= E_IDLE;
            endcase
        end
    end
endmodule

module scopes (
    // system clock
    input wire         clk,
    input wire         rst,
    input wire         enable,             // from the console's control register
    input wire         audio_valid,        // one per colour clock
    input wire [63:0]  voices,             // {voice 3, 2, 1, 0}, signed 16-bit each
    // pixel clock
    input wire         clk_pixel,
    input wire         rst_pixel,
    input wire [9:0]   pixel_x,
    input wire [9:0]   pixel_y,
    output reg         on,
    output reg [23:0]  rgb,
    // for testbenches (leave unconnected): the capture's writes and the slot
    // they go to, on the system clock; the newest slot on screen, on the
    // pixel clock
    output wire [3:0]  dbg_we,
    output wire [1:0]  dbg_slot,
    output wire [8:0]  dbg_index_0, dbg_index_1, dbg_index_2, dbg_index_3,
    output wire [31:0] dbg_points,
    output wire        dbg_start,
    output wire [1:0]  dbg_front
);
    // ---- system clock: capture -------------------------------------------------
    reg  [2:0]  frame_sync;
    reg  [1:0]  front_g1, front_g2;           // the screen's newest slot, Gray coded, synchronised
    reg  [1:0]  latest, latest_gray;          // the newest complete capture
    wire [1:0]  next_slot = latest + 2'd1;
    wire [1:0]  front_seen = {front_g2[1], front_g2[1] ^ front_g2[0]};   // Gray to binary
    wire        new_frame = frame_sync[2] != frame_sync[1];
    wire [7:0]  phases;
    wire        all_idle = phases == 8'h00;
    wire        all_done = phases == 8'hFF;
    // start when the screen has caught up with the last capture
    wire        start = new_frame && all_idle && front_seen == latest && enable;
    assign dbg_start = start;
    assign dbg_slot = next_slot;

    // the Voices' first points, and their segments for the shared engine
    wire [3:0]  c_wen;
    wire [35:0] c_widx;
    wire [27:0] c_wdat;
    wire [3:0]  req, take, done, c_neg;
    wire [43:0] c_gap, c_off, c_q0, c_dq;
    wire [39:0] c_j, c_stop;
    wire [35:0] c_first;                      // each Voice's window: its first column in the ring
    wire [10:0] off_new;
    wire [9:0]  j_new;
    wire        finished;
    // the engine's columns
    wire        e_wen;
    wire [8:0]  e_widx;
    wire [6:0]  e_wdat;
    wire [1:0]  e_wsel;
    scope_engine engine (
        .clk(clk), .rst(rst), .req(req), .g_gap(c_gap), .g_off(c_off), .g_q0(c_q0), .g_neg(c_neg),
        .g_dq(c_dq), .g_j(c_j), .g_stop(c_stop), .take(take), .done(done), .off_out(off_new), .j_out(j_new),
        .finished(finished), .wen(e_wen), .widx(e_widx), .wdat(e_wdat), .wsel(e_wsel)
    );

    // win_first: per Voice and slot ({voice, slot}), the window's first
    // column in the ring. Set as the capture finishes, so like the slot's
    // points it is read on the pixel clock only while the slot isn't being
    // written.
    reg  [8:0] win_first [0:15];
    integer i;

    // what each Voice's memory is written with
    wire [3:0]  wen;
    wire [35:0] widx;
    wire [27:0] wdat;
    genvar g, s;
    generate
        for (g = 0; g < 4; g = g + 1) begin : capture
            scope_capture cap (
                .clk(clk), .rst(rst), .audio_valid(audio_valid), .v(voices[16 * g +: 16]),
                .start(start), .clear(all_done),
                .phase(phases[2 * g +: 2]), .wen(c_wen[g]), .widx(c_widx[9 * g +: 9]), .wdat(c_wdat[7 * g +: 7]),
                .req(req[g]), .s_gap(c_gap[11 * g +: 11]), .s_off(c_off[11 * g +: 11]),
                .s_q0(c_q0[11 * g +: 11]), .s_neg(c_neg[g]), .s_dq(c_dq[11 * g +: 11]),
                .take(take[g]), .j_out(c_j[10 * g +: 10]), .stop(c_stop[10 * g +: 10]),
                .first_col(c_first[9 * g +: 9]),
                .done(done[g]), .off_new(off_new), .j_new(j_new), .finished(finished)
            );
            assign wen[g] = c_wen[g] | (e_wen && e_wsel == g);
            assign widx[9 * g +: 9] = c_wen[g] ? c_widx[9 * g +: 9] : e_widx;
            assign wdat[7 * g +: 7] = c_wen[g] ? c_wdat[7 * g +: 7] : e_wdat;
        end
    endgenerate
    assign dbg_we = wen;
    assign dbg_index_0 = widx[8:0];
    assign dbg_index_1 = widx[17:9];
    assign dbg_index_2 = widx[26:18];
    assign dbg_index_3 = widx[35:27];
    assign dbg_points = {wdat[27], wdat[27:21], wdat[20], wdat[20:14], wdat[13], wdat[13:7], wdat[6], wdat[6:0]};

    always @(posedge clk) begin
        if (rst) begin
            frame_sync <= 3'd0;
            front_g1 <= 2'd0;
            front_g2 <= 2'd0;
            latest <= 2'd0;
            latest_gray <= 2'd0;
            for (i = 0; i < 16; i = i + 1)
                win_first[i] <= 9'd0;
        end else begin
            frame_sync <= {frame_sync[1:0], frame_t};
            front_g1 <= front_gray;
            front_g2 <= front_g1;
            latest_gray <= latest ^ {1'b0, latest[1]};
            if (all_done)
                latest <= next_slot;
            if (all_done)                         // where each Voice's window starts in the slot
                for (i = 0; i < 4; i = i + 1)
                    win_first[{i[1:0], next_slot}] <= c_first[9 * i +: 9];
        end
    end

    // ---- the slots: per Voice, 4 of 320 x 7 (316 used: the RAM comes in 64s) ----------------------------------
    // Written on the system clock, read on the pixel clock: the pixel side
    // reads all four slots of a column at once (words, below). Kept in
    // logic: left to itself XST makes RAMB8s, which ISE 14.7's bitgen
    // mishandles (docs/board.md).
    // Each slot is read at col_s[slot]: the column drawn, moved by the
    // window of the scope being drawn (the other Voices' reads aren't used).
    reg  [8:0] col_s [0:3];
    wire [111:0] words;                       // {voice 3, 2, 1, 0} x {slot 3, 2, 1, 0}, 7 bits each
    generate
        for (g = 0; g < 4; g = g + 1) begin : voice
            for (s = 0; s < 4; s = s + 1) begin : slot
                (* ram_style = "distributed" *) reg [6:0] m [0:319];
                always @(posedge clk)
                    if (wen[g] && next_slot == s)
                        m[widx[9 * g +: 9]] <= wdat[7 * g +: 7];
                assign words[28 * g + 7 * s + 6 : 28 * g + 7 * s] = m[col_s[s]];
            end
        end
    endgenerate

    // ---- pixel clock -----------------------------------------------------------------
    reg [1:0] latest_g1, latest_g2, front, front_gray;
    reg [2:0] en_sync;
    reg       frame_t;
    wire [1:0] latest_seen = {latest_g2[1], latest_g2[1] ^ latest_g2[0]};
    assign dbg_front = front;
    always @(posedge clk_pixel) begin
        if (rst_pixel) begin
            latest_g1 <= 2'd0;
            latest_g2 <= 2'd0;
            en_sync <= 3'd0;
            front <= 2'd0;
            front_gray <= 2'd0;
            frame_t <= 1'b0;
        end else begin
            latest_g1 <= latest_gray;
            latest_g2 <= latest_g1;
            en_sync <= {en_sync[1:0], enable};
            if (pixel_x == 10'd0 && pixel_y == 10'd480) begin
                front <= latest_seen;             // the next frame shows the latest capture
                front_gray <= latest_g2;          // (the same, Gray coded, to tell capture)
                frame_t <= !frame_t;              // and a new capture may start
            end
        end
    end

    function [8:0] ring;                      // column c of a window starting at f, in the ring of 316
        input [8:0] c, f;
        reg [9:0] sum;
        begin
            sum = {1'b0, c} + {1'b0, f};
            ring = sum >= 10'd316 ? sum[8:0] - 9'd316 : sum[8:0];
        end
    endfunction

    // stage A: which scope, which column, how far above its centre line
    wire [9:0] gx = pixel_x - 10'd2;
    wire       right_n = gx >= 10'd320;
    wire [9:0] col_n = right_n ? gx - 10'd320 : gx;
    wire [9:0] yl = pixel_y - 10'd96;                          // 0..191 in the band
    wire       bottom_n = yl >= 10'd96;
    wire [7:0] yband = bottom_n ? yl[7:0] - 8'd96 : yl[7:0];   // 0..95 within the scope
    // the memory is read for the next pixel, so a scope's first column is
    // already in the shift register (below) when its first pixel arrives
    wire [9:0] gxn = pixel_x - 10'd1;                          // the next pixel's gx
    wire       rn = gxn >= 10'd320;
    wire [9:0] cn = rn ? gxn - 10'd320 : gxn;
    reg  [1:0] scope_a, scope_rd, scope_b, scope_c, scope_d;
    reg  signed [8:0] dy_a, dy_b;
    reg        in_a, in_b, first_a, first_b, last_a, last_b;
    always @(posedge clk_pixel) begin
        scope_a <= {bottom_n, right_n};
        scope_rd <= {bottom_n, rn};
        for (i = 0; i < 4; i = i + 1)
            col_s[i] <= ring(cn[8:0], win_first[{bottom_n, rn, i[1:0]}]);
        first_a <= col_n == 10'd0;
        last_a <= col_n == 10'd315;
        dy_a <= 9'sd47 - $signed({1'b0, yband});                // +47 at the top row, -48 at the bottom: the points' range
        in_a <= en_sync[1] && pixel_x >= 10'd2 && pixel_x < 10'd638 && pixel_y >= 10'd96 &&
                pixel_y < 10'd288 && col_n < 10'd316;
    end

    // stage B: the points in every slot of the column of the pixel after
    // this one, which the shift register then carries to be this pixel's and
    // the one before's as the pixels go by (so stage C has all three columns)
    wire [27:0] word = words[28 * scope_rd +: 28];
    reg  [27:0] nxt_c, cur_c, prv_c;
    always @(posedge clk_pixel) begin
        nxt_c <= word;
        cur_c <= nxt_c;
        prv_c <= cur_c;
        scope_b <= scope_a;
        dy_b <= dy_a;
        in_b <= in_a;
        first_b <= first_a;
        last_b <= last_a;
    end
    wire [27:0] wp = first_b ? cur_c : prv_c;     // the first and last columns have
    wire [27:0] wn = last_b ? cur_c : nxt_c;      // no neighbour: use their own point

    // stage C: on the trace of the newest capture, the one before, or the
    // one before that, or the centre line
    function on_trace;
        input [27:0] pw, cw, nw;               // the column before, this and the column after
        input [1:0]  slot;
        input signed [8:0] dy;
        reg signed [7:0] p, c, n;
        reg signed [9:0] ml, mr, c2, lo, hi, d2;     // in half pixels
        begin
            p = {pw[7 * slot + 6], pw[7 * slot +: 7]};
            c = {cw[7 * slot + 6], cw[7 * slot +: 7]};
            n = {nw[7 * slot + 6], nw[7 * slot +: 7]};
            ml = p + c;
            mr = c + n;
            c2 = c + c;
            lo = ml < c2 ? ml : c2;
            if (mr < lo) lo = mr;
            hi = ml > c2 ? ml : c2;
            if (mr > hi) hi = mr;
            d2 = dy + dy;
            on_trace = d2 >= lo && d2 <= hi;
        end
    endfunction
    // How many captures are shown: the newest only (the owner found three
    // overlaid traces looked like three waveforms, not persistence); 2 or 3
    // add the ones before it at three quarters and half brightness.
    localparam GENERATIONS = 1;
    reg  [1:0] level_c, level_d;              // 3 newest, 2 three quarters, 1 half, 0 none
    reg        centre_c, centre_d;
    always @(posedge clk_pixel) begin
        level_c <= !in_b ? 2'd0 :
                   on_trace(wp, cur_c, wn, front, dy_b) ? 2'd3 :
                   (GENERATIONS > 1 && on_trace(wp, cur_c, wn, front - 2'd1, dy_b)) ? 2'd2 :
                   (GENERATIONS > 2 && on_trace(wp, cur_c, wn, front - 2'd2, dy_b)) ? 2'd1 : 2'd0;
        centre_c <= in_b && dy_b == 9'sd0;
        scope_c <= scope_b;
        // stage D
        level_d <= level_c;
        centre_d <= centre_c;
        scope_d <= scope_c;
    end

    // stage E: the colour, 5 clocks after pixel_x / pixel_y
    wire [23:0] base = 24'hF4F4F4;            // white, like a vacuum fluorescent display (ADR-0016)
    wire [23:0] half    = {1'b0, base[23:17], 1'b0, base[15:9], 1'b0, base[7:1]};
    wire [23:0] quarter = {2'b0, base[23:18], 2'b0, base[15:10], 2'b0, base[7:2]};
    always @(posedge clk_pixel) begin
        on <= level_d != 2'd0 || centre_d;
        rgb <= level_d == 2'd3 ? base :
               level_d == 2'd2 ? half + quarter :      // three quarters (no carry between bytes: 127 + 63 < 256)
               level_d == 2'd1 ? half :
               24'h303030;                             // centre line
    end
endmodule
`default_nettype wire
