// The SoC and the HDMI output together, for console_tb.cpp: the CPU's
// external bus drives the text console as in the HDMI board build (top.v),
// with the two clocks brought out so the testbench can run them at their
// real ratio.
`default_nettype none
module console_wrap #(
    parameter FW0 = "firmware0.hex",
    parameter FW1 = "firmware1.hex",
    parameter FW2 = "firmware2.hex",
    parameter FW3 = "firmware3.hex",
    parameter COEFS = "resampler_coefs.hex",
    parameter FONT = "console_font.hex"
) (
    input wire         clk,
    input wire         rst,
    input wire         clk_pixel,
    input wire         rst_pixel,
    output wire        uart_tx,
    input wire         uart_rx,
    output wire        sd_cke, sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n,
    output wire [1:0]  sd_ba,
    output wire [12:0] sd_a,
    output wire [1:0]  sd_dqm,
    output wire [15:0] sd_dq_o,
    output wire        sd_dq_oe,
    input wire  [15:0] sd_dq_i,
    output wire [9:0]  tmds0, tmds1, tmds2,
    output wire        dbg_text_we,
    output wire [11:0] dbg_text_cell,
    output wire [15:0] dbg_text_data,
    output wire [1:0]  dbg_text_be
);
    wire        audio_valid, ext_valid, ext_ready;
    wire signed [15:0] audio_l, audio_r;
    wire [14:2] ext_addr;
    wire [31:0] ext_wdata, ext_rdata;
    wire [3:0]  ext_wstrb;
    wire [7:0]  leds;

    soc #(.FW0(FW0), .FW1(FW1), .FW2(FW2), .FW3(FW3)) soc (
        .clk(clk), .rst(rst), .uart_tx(uart_tx), .uart_rx(uart_rx), .leds(leds), .switches(4'd0),
        .sd_cke(sd_cke), .sd_cs_n(sd_cs_n), .sd_ras_n(sd_ras_n), .sd_cas_n(sd_cas_n), .sd_we_n(sd_we_n),
        .sd_ba(sd_ba), .sd_a(sd_a), .sd_dqm(sd_dqm), .sd_dq_o(sd_dq_o), .sd_dq_oe(sd_dq_oe), .sd_dq_i(sd_dq_i),
        .audio_valid(audio_valid), .audio_l(audio_l), .audio_r(audio_r),
        .flash_cs_n(), .flash_sck(), .flash_mosi(), .flash_miso(1'b1),     // an erased flash
        .ext_valid(ext_valid), .ext_addr(ext_addr), .ext_wdata(ext_wdata), .ext_wstrb(ext_wstrb),
        .ext_ready(ext_ready), .ext_rdata(ext_rdata),
        .dbg_paula_we(), .dbg_paula_addr(), .dbg_paula_wdata()
    );

    hdmi_out #(.COEFS(COEFS), .FONT(FONT)) hdmi (
        .clk(clk), .rst(rst),
        .audio_valid(audio_valid), .audio_l(audio_l), .audio_r(audio_r),
        .text_valid(ext_valid), .text_addr(ext_addr), .text_wdata(ext_wdata[15:0]), .text_wstrb(ext_wstrb),
        .text_ready(ext_ready), .text_rdata(ext_rdata),
        .clk_pixel(clk_pixel), .rst_pixel(rst_pixel),
        .tmds0(tmds0), .tmds1(tmds1), .tmds2(tmds2),
        .resampler_overrun(), .fifo_overflow(), .fifo_underflow(), .fifo_primed(),
        .dbg_text_we(dbg_text_we), .dbg_text_cell(dbg_text_cell), .dbg_text_data(dbg_text_data),
        .dbg_text_be(dbg_text_be)
    );
endmodule
`default_nettype wire
