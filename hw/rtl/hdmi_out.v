// HDMI output, clock-independent part (ADR-0010): Paula's frames in on the
// system clock, TMDS characters out on the pixel clock. The board top level
// adds the clocks and the serialiser.
//
//   Paula frames -> Resampler (48 kHz) -> dual-clock FIFO -> hdmi_tx
//   CPU writes -> text console (53 x 20 characters) -> hdmi_tx's picture,
//   with the Voice scopes (from Paula's per-Voice output) and the spectrum
//   analyser (from the Resampler's output) drawn over it
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
module hdmi_out #(
    parameter COEFS = "resampler_coefs.hex",
    parameter FONT  = "console_font.hex",
    parameter SEGMENTS = "console_segments.hex",
    parameter SPECTRUM_WINDOW = "spectrum_window.hex",
    parameter SPECTRUM_SINE   = "spectrum_sine.hex",
    parameter SPECTRUM_BARS   = "spectrum_bars.hex"
) (
    // system clock: Paula's frames, one per colour clock
    input wire               clk,
    input wire               rst,
    input wire               audio_valid,
    input wire signed [15:0] audio_l,
    input wire signed [15:0] audio_r,
    input wire [63:0]        audio_voices,      // each Voice's part, for the scopes
    // system clock: the text console's bus (hw/rtl/text_console.v)
    input wire               text_valid,
    input wire [14:2]        text_addr,
    input wire [15:0]        text_wdata,
    input wire [3:0]         text_wstrb,
    output wire              text_ready,
    output wire [31:0]       text_rdata,
    // pixel clock (25.2 MHz)
    input wire               clk_pixel,
    input wire               rst_pixel,
    output wire [9:0]        tmds0,
    output wire [9:0]        tmds1,
    output wire [9:0]        tmds2,
    // status (sticky except primed): for tests and an LED
    output wire              resampler_overrun,
    output wire              fifo_overflow,
    output wire              fifo_underflow,
    output wire              fifo_primed,
    // the text RAM's writes, on the pixel clock, for testbenches
    output wire              dbg_text_we,
    output wire [11:0]       dbg_text_cell,
    output wire [15:0]       dbg_text_data,
    output wire [1:0]        dbg_text_be,
    // the scopes' capture (system clock) and the half shown (pixel clock), for testbenches
    output wire [3:0]        dbg_scope_we,
    output wire [1:0]        dbg_scope_slot,
    output wire [35:0]       dbg_scope_index,   // {voice 3, 2, 1, 0}, 9 bits each
    output wire [31:0]       dbg_scope_points,
    output wire              dbg_scope_start,
    output wire [1:0]        dbg_scope_front,
    output wire [7:0]        dbg_control,       // the console's control register
    // the spectrum's bars and peaks (system clock) and its display copy (pixel clock)
    output wire [191:0]      dbg_spectrum_bars,
    output wire [191:0]      dbg_spectrum_peaks,
    output wire              dbg_spectrum_copy
);
    wire               rs_valid;
    wire signed [23:0] rs_l, rs_r;
    resampler #(.COEFS(COEFS)) resampler (
        .clk(clk), .rst(rst),
        .in_valid(audio_valid), .in_l(audio_l), .in_r(audio_r),
        .out_valid(rs_valid), .out_l(rs_l), .out_r(rs_r),
        .overrun(resampler_overrun)
    );

    wire               take;
    wire signed [23:0] hd_l, hd_r;
    hdmi_audio_fifo fifo (
        .wclk(clk), .wrst(rst), .w_valid(rs_valid), .w_l(rs_l), .w_r(rs_r), .overflow(fifo_overflow),
        .rclk(clk_pixel), .rrst(rst_pixel), .r_take(take), .r_l(hd_l), .r_r(hd_r),
        .primed(fifo_primed), .underflow(fifo_underflow)
    );

    wire [9:0]  px, py;
    wire [23:0] rgb, scope_rgb, spectrum_rgb;
    wire        spectrum_on;
    wire [7:0]  control;
    wire        scope_on;
    assign dbg_control = control;
    text_console #(.FONT(FONT), .SEGMENTS(SEGMENTS)) console (
        .clk(clk), .rst(rst),
        .bus_valid(text_valid), .bus_addr(text_addr), .bus_wdata(text_wdata), .bus_wstrb(text_wstrb),
        .bus_ready(text_ready), .bus_rdata(text_rdata), .control(control),
        .clk_pixel(clk_pixel), .rst_pixel(rst_pixel), .pixel_x(px), .pixel_y(py), .rgb(rgb),
        .dbg_we(dbg_text_we), .dbg_cell(dbg_text_cell), .dbg_data(dbg_text_data), .dbg_be(dbg_text_be)
    );

    scopes scopes (
        .clk(clk), .rst(rst), .enable(control[0]),
        .audio_valid(audio_valid), .voices(audio_voices),
        .clk_pixel(clk_pixel), .rst_pixel(rst_pixel), .pixel_x(px), .pixel_y(py),
        .on(scope_on), .rgb(scope_rgb),
        .dbg_we(dbg_scope_we), .dbg_slot(dbg_scope_slot),
        .dbg_index_0(dbg_scope_index[8:0]), .dbg_index_1(dbg_scope_index[17:9]),
        .dbg_index_2(dbg_scope_index[26:18]), .dbg_index_3(dbg_scope_index[35:27]),
        .dbg_points(dbg_scope_points), .dbg_start(dbg_scope_start), .dbg_front(dbg_scope_front)
    );

    spectrum #(.WINDOW(SPECTRUM_WINDOW), .SINE(SPECTRUM_SINE), .BARS(SPECTRUM_BARS)) spectrum (
        .clk(clk), .rst(rst), .enable(control[1]),
        .in_valid(rs_valid), .in_l(rs_l), .in_r(rs_r),
        .clk_pixel(clk_pixel), .rst_pixel(rst_pixel), .pixel_x(px), .pixel_y(py),
        .on(spectrum_on), .rgb(spectrum_rgb),
        .dbg_start(), .dbg_done(), .dbg_bars(dbg_spectrum_bars), .dbg_peaks(dbg_spectrum_peaks),
        .dbg_copy(dbg_spectrum_copy)
    );

    // the scopes and the spectrum over the text: all are 5 clocks behind the timing
    hdmi_tx #(.RGB_DELAY(5)) tx (
        .clk(clk_pixel), .rst(rst_pixel),
        .audio_tick(take), .audio_l(hd_l), .audio_r(hd_r),
        .pixel_x(px), .pixel_y(py), .rgb(scope_on ? scope_rgb : spectrum_on ? spectrum_rgb : rgb),
        .tmds0(tmds0), .tmds1(tmds1), .tmds2(tmds2)
    );
endmodule
