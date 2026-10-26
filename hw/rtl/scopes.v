// Four oscilloscope traces on the HDMI picture, one per Voice (issue #23,
// ADR-0013), drawn from Paula's own per-Voice output (sample x volume), so
// they cost the CPU nothing. They sit in a 2 x 2 grid in rows 4-11 of the
// text console (y 96..287): Voices 1 and 2 on top, 3 and 4 below, each 316
// pixels wide and 96 tall, the left ones from x = 2 and the right ones from
// x = 322 (a 4-pixel gap between); the console's text shows through
// wherever there is no trace.
//
// Capture (system clock): once a frame, each Voice waits for its output to
// cross zero going up (as a scope triggers; or gives up after 10 ms, so
// notes down to about 99 Hz trigger), then takes 316 points, one every 64
// colour clocks (5.7 ms in all), each the Voice's output >> 7 times 0.75
// (p - (p >> 2): -48..48, so a full-scale Voice fills its scope; 7 bits).
//
// Persistence: each Voice keeps its last captures in a ring of 4 slots.
// The screen shows the newest at full brightness and the two before it at
// half and a quarter, like a scope's phosphor fading; the fourth slot is
// the one being captured into. A capture fills the slot after the newest;
// when all four Voices are done that slot becomes the newest, and the
// screen takes it at the next frame. A new capture starts only once the
// screen has taken the last one, so the slot being written is never on
// screen.
//
// Display (pixel clock): point i of a capture is column i of its scope, its
// value pixels above the scope's centre line; each column is lit from its
// point to the previous column's, joining the points up. A dim centre
// line runs through each scope. Like the console, the colour for pixel
// (pixel_x, pixel_y) is ready 5 clocks later: on = this pixel is a scope's.
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
`default_nettype none
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
    // The wait plus a capture (315 x 64 colour clocks) must fit in a video
    // frame (59115) with room to spare, so captures keep up with the screen.
    localparam POINTS = 316, STRIDE_BITS = 6, TIMEOUT = 16'd36000;

    // ---- system clock: capture -------------------------------------------------
    reg  [2:0]  frame_sync;
    reg  [1:0]  front_g1, front_g2;           // the screen's newest slot, Gray coded, synchronised
    reg  [1:0]  latest, latest_gray;          // the newest complete capture
    reg  [1:0]  phase [0:3];                  // 0 idle, 1 waiting, 2 capturing, 3 done
    reg  [8:0]  index [0:3];
    reg  [STRIDE_BITS-1:0] count [0:3];
    reg  [15:0] waited [0:3];
    reg  [3:0]  was_negative;
    wire [1:0]  next_slot = latest + 2'd1;
    wire [1:0]  front_seen = {front_g2[1], front_g2[1] ^ front_g2[0]};   // Gray to binary
    wire        new_frame = frame_sync[2] != frame_sync[1];
    wire        all_idle = phase[0] == 2'd0 && phase[1] == 2'd0 && phase[2] == 2'd0 && phase[3] == 2'd0;
    wire        all_done = phase[0] == 2'd3 && phase[1] == 2'd3 && phase[2] == 2'd3 && phase[3] == 2'd3;
    // start when the screen has caught up with the last capture
    wire        start = new_frame && all_idle && front_seen == latest && enable;
    assign dbg_start = start;
    assign dbg_slot = next_slot;

    // the points: each Voice's output >> 7, times 0.75
    wire [7:0]  point [0:3];
    wire [3:0]  write;
    genvar g, s;
    generate
        for (g = 0; g < 4; g = g + 1) begin : capture
            wire signed [15:0] v = voices[16 * g + 15 : 16 * g];
            wire [7:0] p = v[14:7];
            assign point[g] = p - {p[7], p[7], p[7:2]};
            wire trigger = was_negative[g] && !v[15];
            assign write[g] = audio_valid && ((phase[g] == 2'd1 && (trigger || waited[g] == TIMEOUT)) ||
                                              (phase[g] == 2'd2 && count[g] == 0));
        end
    endgenerate
    assign dbg_we = write;
    assign dbg_index_0 = index[0];
    assign dbg_index_1 = index[1];
    assign dbg_index_2 = index[2];
    assign dbg_index_3 = index[3];
    assign dbg_points = {point[3], point[2], point[1], point[0]};

    integer i;
    always @(posedge clk) begin
        if (rst) begin
            frame_sync <= 3'd0;
            front_g1 <= 2'd0;
            front_g2 <= 2'd0;
            latest <= 2'd0;
            latest_gray <= 2'd0;
            was_negative <= 4'd0;
            for (i = 0; i < 4; i = i + 1) begin
                phase[i] <= 2'd0;
                index[i] <= 9'd0;
                count[i] <= 0;
                waited[i] <= 16'd0;
            end
        end else begin
            frame_sync <= {frame_sync[1:0], frame_t};
            front_g1 <= front_gray;
            front_g2 <= front_g1;
            latest_gray <= latest ^ {1'b0, latest[1]};
            if (start)
                for (i = 0; i < 4; i = i + 1) begin
                    phase[i] <= 2'd1;
                    waited[i] <= 16'd0;
                end
            else if (all_done) begin
                latest <= next_slot;
                for (i = 0; i < 4; i = i + 1)
                    phase[i] <= 2'd0;
            end else if (audio_valid)
                for (i = 0; i < 4; i = i + 1) begin
                    if (phase[i] == 2'd1) begin
                        waited[i] <= waited[i] + 1'b1;
                        if (write[i]) begin
                            phase[i] <= 2'd2;
                            index[i] <= 9'd1;
                            count[i] <= 1;
                        end
                    end else if (phase[i] == 2'd2) begin
                        count[i] <= count[i] + 1'b1;
                        if (write[i]) begin
                            index[i] <= index[i] + 1'b1;
                            if (index[i] == POINTS - 1)
                                phase[i] <= 2'd3;
                        end
                    end
                end
            if (start)
                for (i = 0; i < 4; i = i + 1)
                    index[i] <= 9'd0;
            if (audio_valid)                      // the sign each frame, for the trigger
                for (i = 0; i < 4; i = i + 1)
                    was_negative[i] <= voices[16 * i + 15];
        end
    end

    // ---- the slots: per Voice, 4 of 512 x 7 (316 used) ----------------------------------
    // Written on the system clock, read on the pixel clock: the pixel side
    // reads all four slots of a column at once (words, below). Kept in
    // logic: left to itself XST makes RAMB8s, which ISE 14.7's bitgen
    // mishandles (docs/board.md).
    reg  [8:0] col_a;
    wire [111:0] words;                       // {voice 3, 2, 1, 0} x {slot 3, 2, 1, 0}, 7 bits each
    generate
        for (g = 0; g < 4; g = g + 1) begin : voice
            for (s = 0; s < 4; s = s + 1) begin : slot
                (* ram_style = "distributed" *) reg [6:0] m [0:511];
                always @(posedge clk)
                    if (write[g] && next_slot == s)
                        m[index[g]] <= point[g][6:0];
                assign words[28 * g + 7 * s + 6 : 28 * g + 7 * s] = m[col_a];
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

    // stage A: which scope, which column, how far above its centre line
    wire [9:0] gx = pixel_x - 10'd2;
    wire       right_n = gx >= 10'd320;
    wire [9:0] col_n = right_n ? gx - 10'd320 : gx;
    wire [9:0] yl = pixel_y - 10'd96;                          // 0..191 in the band
    wire       bottom_n = yl >= 10'd96;
    wire [7:0] yband = bottom_n ? yl[7:0] - 8'd96 : yl[7:0];   // 0..95 within the scope
    reg  [1:0] scope_a, scope_b, scope_c, scope_d;
    reg  signed [8:0] dy_a, dy_b;
    reg        in_a, in_b;
    always @(posedge clk_pixel) begin
        scope_a <= {bottom_n, right_n};
        col_a <= col_n[8:0];
        dy_a <= 9'sd48 - $signed({1'b0, yband});
        in_a <= en_sync[1] && pixel_x >= 10'd2 && pixel_x < 10'd638 && pixel_y >= 10'd96 &&
                pixel_y < 10'd288 && col_n < 10'd316;
    end

    // stage B: the column's points in every slot, and the column before's
    wire [27:0] word = words[28 * scope_a +: 28];
    reg  [27:0] here_b, before_b;
    always @(posedge clk_pixel) begin
        before_b <= col_a == 9'd0 ? word : here_b;
        here_b <= word;
        scope_b <= scope_a;
        dy_b <= dy_a;
        in_b <= in_a;
    end

    // stage C: on the trace of the newest capture, the one before, or the
    // one before that (between this point and the last), or the centre line
    function on_trace;
        input [27:0] this_col, last_col;
        input [1:0]  slot;
        input signed [8:0] dy;
        reg signed [7:0] h, b, lo, hi;
        begin
            h = {this_col[7 * slot + 6], this_col[7 * slot +: 7]};
            b = {last_col[7 * slot + 6], last_col[7 * slot +: 7]};
            lo = h < b ? h : b;
            hi = h < b ? b : h;
            on_trace = dy >= lo && dy <= hi;
        end
    endfunction
    reg  [1:0] level_c, level_d;              // 3 newest, 2 half, 1 quarter, 0 none
    reg        centre_c, centre_d;
    always @(posedge clk_pixel) begin
        level_c <= !in_b ? 2'd0 :
                   on_trace(here_b, before_b, front, dy_b) ? 2'd3 :
                   on_trace(here_b, before_b, front - 2'd1, dy_b) ? 2'd2 :
                   on_trace(here_b, before_b, front - 2'd2, dy_b) ? 2'd1 : 2'd0;
        centre_c <= in_b && dy_b == 9'sd0;
        scope_c <= scope_b;
        // stage D
        level_d <= level_c;
        centre_d <= centre_c;
        scope_d <= scope_c;
    end

    // stage E: the colour, 5 clocks after pixel_x / pixel_y
    wire [23:0] base = scope_d == 2'd0 ? 24'h55FF55 : scope_d == 2'd1 ? 24'h55FFFF :
                       scope_d == 2'd2 ? 24'hFFFF55 : 24'hFF55FF;   // light green, cyan, yellow, magenta
    always @(posedge clk_pixel) begin
        on <= level_d != 2'd0 || centre_d;
        rgb <= level_d == 2'd3 ? base :
               level_d == 2'd2 ? {1'b0, base[23:17], 1'b0, base[15:9], 1'b0, base[7:1]} :     // half
               level_d == 2'd1 ? {2'b0, base[23:18], 2'b0, base[15:10], 2'b0, base[7:2]} :    // quarter
               24'h555555;                                                                    // centre line
    end
endmodule
`default_nettype wire
