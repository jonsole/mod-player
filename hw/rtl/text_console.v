// Text console for the HDMI picture (issue #21, ADR-0012): 53 x 20 cells of
// 12 x 24 pixels (Spleen, console_font.hex), each a character in code page
// 437 and an attribute: low nibble the foreground colour, high nibble the
// background, from the 16 VGA colours. The grid is 636 pixels wide, so a
// 2-pixel black border is left at each side. Cell 0 is the top left; cell n
// is row n / 53, column n % 53.
//
// System clock side, a small bus (the SoC's 0x2001_0000 window):
//   addr[14] = 0: cell addr[13:2] (0..1059), write only: data[7:0] character,
//                 data[15:8] attribute (byte enables respected)
//   addr[14] = 1: read: bit 31 = 1 (a console is present), bit 30 = in the
//                 vertical blanking, bits 23:0 = frames shown so far;
//                 write: the control register, data[7:0] (bit 0: show the
//                 Voice scopes, hw/rtl/scopes.v)
// A write is held in a register and handed to the pixel clock, which writes
// the text RAM; the next write waits until it has (about 150 ns). So the RAM
// is only ever written and read on one clock: no read can see a half-written
// cell. Firmware that wants a screen never to show half an update writes it
// in the vertical blanking (1.4 ms), or watches the frame count.
//
// Pixel clock side: pixel_x / pixel_y from hdmi_tx; the colour for that
// pixel comes out on rgb 5 clocks later (hdmi_tx's RGB_DELAY).
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
`default_nettype none
module text_console #(
    parameter FONT = "console_font.hex"
) (
    // system clock
    input wire         clk,
    input wire         rst,
    input wire         bus_valid,
    input wire [14:2]  bus_addr,
    input wire [15:0]  bus_wdata,
    input wire [3:0]   bus_wstrb,
    output wire        bus_ready,
    output wire [31:0] bus_rdata,
    output reg  [7:0]  control,            // the control register (system clock)
    // pixel clock
    input wire         clk_pixel,
    input wire         rst_pixel,
    input wire [9:0]   pixel_x,
    input wire [9:0]   pixel_y,
    output reg [23:0]  rgb,
    // the pixel-clock write, for testbenches (leave unconnected)
    output wire        dbg_we,
    output wire [11:0] dbg_cell,
    output wire [15:0] dbg_data,
    output wire [1:0]  dbg_be
);
    // ---- system clock: one write in flight -------------------------------------
    reg        req_t;                  // toggles for each write handed over
    reg [11:0] w_cell;
    reg [15:0] w_data;
    reg [1:0]  w_be;
    reg [2:0]  ack_sync;               // the pixel side's ack_t, synchronised
    wire       busy = req_t != ack_sync[1];
    wire       is_write = bus_wstrb != 4'd0;
    wire       cell_write = bus_valid && is_write && !bus_addr[14];
    assign     bus_ready = bus_valid && !(cell_write && busy);

    reg [2:0]  frame_sync, vblank_sync;
    reg [23:0] frames;
    assign bus_rdata = bus_addr[14] ? {1'b1, vblank_sync[1], 6'd0, frames} : 32'd0;

    always @(posedge clk) begin
        if (rst) begin
            req_t <= 1'b0;
            ack_sync <= 3'd0;
            frame_sync <= 3'd0;
            vblank_sync <= 3'd0;
            frames <= 24'd0;
            control <= 8'd0;
        end else begin
            if (bus_valid && is_write && bus_addr[14])
                control <= bus_wdata[7:0];
            ack_sync <= {ack_sync[1:0], ack_t};
            frame_sync <= {frame_sync[1:0], frame_t};
            vblank_sync <= {vblank_sync[1:0], vblank};
            if (frame_sync[2] != frame_sync[1])
                frames <= frames + 1'b1;
            if (cell_write && !busy) begin
                w_cell <= bus_addr[13:2];
                w_data <= bus_wdata;
                w_be   <= bus_wstrb[1:0];
                req_t  <= !req_t;
            end
        end
    end

    // ---- pixel clock: apply writes, count frames ----------------------------------
    reg [2:0] req_sync;
    reg       ack_t, frame_t, vblank;
    wire      take = req_sync[2] != req_sync[1];
    assign dbg_we = take;
    assign dbg_cell = w_cell;
    assign dbg_data = w_data;
    assign dbg_be = w_be;

    always @(posedge clk_pixel) begin
        if (rst_pixel) begin
            req_sync <= 3'd0;
            ack_t <= 1'b0;
            frame_t <= 1'b0;
            vblank <= 1'b0;
        end else begin
            req_sync <= {req_sync[1:0], req_t};
            if (take)
                ack_t <= !ack_t;        // w_* were stable before req_t changed
            if (pixel_x == 10'd0 && pixel_y == 10'd480)
                frame_t <= !frame_t;    // a frame has just been shown
            vblank <= pixel_y >= 10'd480;
        end
    end

    // ---- stage A: which cell, and where in it -------------------------------------
    // x / 12 and y / 24 as multiplications: exact for x < 636, y < 480
    wire [9:0]  gx = pixel_x - 10'd2;                      // grid x
    wire [22:0] col_m = gx * 13'd5462;
    wire [21:0] row_m = pixel_y * 12'd2731;
    wire [5:0]  col_n = col_m[21:16];
    wire [4:0]  row_n = row_m[20:16];
    reg  [5:0]  col_a;
    reg  [4:0]  row_a, gl_a, gl_b, gl_c;
    reg  [3:0]  sub_a, sub_b, sub_c, sub_d;
    reg         in_a, in_b, in_c, in_d;
    reg  [11:0] cell_b;
    reg  [7:0]  ch_c, at_c, at_d;
    reg  [11:0] glyph_d;

    always @(posedge clk_pixel) begin
        col_a <= col_n;
        row_a <= row_n;
        sub_a <= gx[3:0] - {col_n[1:0], 2'b00} - {col_n[2:0], 3'b000};   // gx - col * 12, low bits
        gl_a  <= pixel_y[4:0] - {row_n[1:0], 3'b000} - {row_n[0], 4'b0000};  // y - row * 24, low bits
        in_a  <= pixel_x >= 10'd2 && pixel_x < 10'd638 && pixel_y < 10'd480;
        // stage B: the cell's address
        cell_b <= {2'd0, row_a, 5'd0} + {3'd0, row_a, 4'd0} + {5'd0, row_a, 2'd0} +
                  {7'd0, row_a} + {6'd0, col_a};          // row * (32 + 16 + 4 + 1) + col
        gl_b <= gl_a;
        sub_b <= sub_a;
        in_b <= in_a;
        // stage C: text RAM read (below); stage D: font read (below)
        gl_c <= gl_b;
        sub_c <= sub_b;
        in_c <= in_b;
        sub_d <= sub_c;
        in_d <= in_c;
        at_d <= at_c;
    end

    // ---- text RAM: 4096 cells (1060 used), written and read on the pixel clock ----
    // Starts all zero (block RAM's power-on state): blank, black on black.
    reg [7:0] chars [0:4095];
    reg [7:0] attrs [0:4095];
    always @(posedge clk_pixel) begin
        if (take && w_be[0]) chars[w_cell] <= w_data[7:0];
        ch_c <= chars[cell_b];
    end
    always @(posedge clk_pixel) begin
        if (take && w_be[1]) attrs[w_cell] <= w_data[15:8];
        at_c <= attrs[cell_b];
    end

    // ---- font: 256 glyphs x 32 rows (24 used) of 12 pixels -------------------------
    reg [11:0] font [0:8191];
    initial $readmemh(FONT, font);
    always @(posedge clk_pixel)
        glyph_d <= font[{ch_c, gl_c}];

    // ---- the 16 VGA colours ----------------------------------------------------------
    function [23:0] palette;
        input [3:0] c;
        case (c)
            4'h0: palette = 24'h000000; 4'h1: palette = 24'h0000AA;
            4'h2: palette = 24'h00AA00; 4'h3: palette = 24'h00AAAA;
            4'h4: palette = 24'hAA0000; 4'h5: palette = 24'hAA00AA;
            4'h6: palette = 24'hAA5500; 4'h7: palette = 24'hAAAAAA;
            4'h8: palette = 24'h555555; 4'h9: palette = 24'h5555FF;
            4'hA: palette = 24'h55FF55; 4'hB: palette = 24'h55FFFF;
            4'hC: palette = 24'hFF5555; 4'hD: palette = 24'hFF55FF;
            4'hE: palette = 24'hFFFF55; default: palette = 24'hFFFFFF;
        endcase
    endfunction

    // stage E: the pixel's colour; rgb holds it 5 clocks after pixel_x / pixel_y
    always @(posedge clk_pixel)
        rgb <= !in_d ? 24'h000000 : glyph_d[4'd11 - sub_d] ? palette(at_d[3:0]) : palette(at_d[7:4]);
endmodule
`default_nettype wire
