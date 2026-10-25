// HDMI output, clock-independent part (ADR-0010): Paula's frames in on the
// system clock, TMDS characters out on the pixel clock. The board top level
// adds the clocks and the serialiser.
//
//   Paula frames -> Resampler (48 kHz) -> dual-clock FIFO -> hdmi_tx
//   CPU writes -> text console (53 x 20 characters) -> hdmi_tx's picture
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
module hdmi_out #(
    parameter COEFS = "resampler_coefs.hex",
    parameter FONT  = "console_font.hex"
) (
    // system clock: Paula's frames, one per colour clock
    input wire               clk,
    input wire               rst,
    input wire               audio_valid,
    input wire signed [15:0] audio_l,
    input wire signed [15:0] audio_r,
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
    output wire [1:0]        dbg_text_be
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
    wire [23:0] rgb;
    text_console #(.FONT(FONT)) console (
        .clk(clk), .rst(rst),
        .bus_valid(text_valid), .bus_addr(text_addr), .bus_wdata(text_wdata), .bus_wstrb(text_wstrb),
        .bus_ready(text_ready), .bus_rdata(text_rdata),
        .clk_pixel(clk_pixel), .rst_pixel(rst_pixel), .pixel_x(px), .pixel_y(py), .rgb(rgb),
        .dbg_we(dbg_text_we), .dbg_cell(dbg_text_cell), .dbg_data(dbg_text_data), .dbg_be(dbg_text_be)
    );

    hdmi_tx #(.RGB_DELAY(5)) tx (
        .clk(clk_pixel), .rst(rst_pixel),
        .audio_tick(take), .audio_l(hd_l), .audio_r(hd_r),
        .pixel_x(px), .pixel_y(py), .rgb(rgb),
        .tmds0(tmds0), .tmds1(tmds1), .tmds2(tmds2)
    );
endmodule
