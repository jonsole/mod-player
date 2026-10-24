// Top level for the Scarab miniSpartan6+ (XC6SLX25): pins, clocks, reset,
// SDRAM I/O and audio around the SoC. Pin names match top.ucf.
//
// Board wiring (from Scarab's Rev B schematic and pin file):
//   CLK50       50 MHz oscillator
//   FTDI_D0/D1  FT2232H channel B: D0 = its TXD (our serial input),
//               D1 = its RXD (our serial output)
//   AUDIO2/1    jack tip (left) / ring (right), each through 3.3 kOhm + 4.7 nF
//   SW1..4      DIP switches to ground, with pull-ups: on = 0
//   LEDS        active high
module top #(
    parameter FIRMWARE = "firmware"       // $readmemh files firmware0.hex .. firmware3.hex
) (
    input         CLK50,
    output [7:0]  LEDS,
    input  [4:1]  SW,
    input         FTDI_D0,
    output        FTDI_D1,
    output        AUDIO1,                 // right
    output        AUDIO2,                 // left
    // SDRAM
    output        SDRAM_CLK,
    output        SDRAM_CKE,
    output        SDRAM_CSn,
    output        SDRAM_RASn,
    output        SDRAM_CASn,
    output        SDRAM_WEn,
    output [1:0]  SDRAM_BA,
    output [12:0] SDRAM_A,
    output        SDRAM_LDQM,
    output        SDRAM_UDQM,
    inout  [15:0] SDRAM_D
);
    // ---- clock ----
    wire clk;
    BUFG clk_buf (.I(CLK50), .O(clk));

    // ---- reset: held for 2^16 clocks (1.3 ms) after configuration ----
    reg [16:0] por = 17'd0;
    always @(posedge clk)
        if (!por[16]) por <= por + 1'b1;
    wire rst = !por[16];

    // ---- SoC ----
    wire        sd_dq_oe;
    wire [15:0] sd_dq_o, sd_dq_i;
    wire        audio_valid;
    wire signed [15:0] audio_l, audio_r;
    soc #(.FIRMWARE(FIRMWARE)) soc (
        .clk(clk), .rst(rst),
        .uart_tx(FTDI_D1), .uart_rx(FTDI_D0),
        .leds(LEDS), .switches(~SW),
        .sd_cke(SDRAM_CKE), .sd_cs_n(SDRAM_CSn), .sd_ras_n(SDRAM_RASn), .sd_cas_n(SDRAM_CASn),
        .sd_we_n(SDRAM_WEn), .sd_ba(SDRAM_BA), .sd_a(SDRAM_A), .sd_dqm({SDRAM_UDQM, SDRAM_LDQM}),
        .sd_dq_o(sd_dq_o), .sd_dq_oe(sd_dq_oe), .sd_dq_i(sd_dq_i),
        .audio_valid(audio_valid), .audio_l(audio_l), .audio_r(audio_r),
        .dbg_paula_we(), .dbg_paula_addr(), .dbg_paula_wdata()
    );

    // ---- SDRAM clock: the system clock inverted, from a DDR output register,
    // so commands and data are sampled half a cycle after they change ----
    ODDR2 #(.DDR_ALIGNMENT("NONE"), .INIT(1'b0), .SRTYPE("SYNC")) sdram_clk_out (
        .Q(SDRAM_CLK), .C0(clk), .C1(~clk), .CE(1'b1), .D0(1'b0), .D1(1'b1), .R(1'b0), .S(1'b0)
    );

    // ---- SDRAM data pins ----
    genvar i;
    generate
        for (i = 0; i < 16; i = i + 1) begin : dq
            IOBUF dq_buf (.IO(SDRAM_D[i]), .I(sd_dq_o[i]), .O(sd_dq_i[i]), .T(!sd_dq_oe));
        end
    endgenerate

    // ---- audio: Paula's frames, doubled to full scale, to the delta-sigma DACs ----
    function [15:0] double_sat;           // x * 2, saturated
        input signed [15:0] x;
        double_sat = (x > 16383) ? 16'sd32767 : (x < -16384) ? -16'sd32768 : {x[14:0], 1'b0};
    endfunction
    reg [15:0] left, right;
    always @(posedge clk)
        if (audio_valid) begin
            left <= double_sat(audio_l);
            right <= double_sat(audio_r);
        end
    dac_dsm dac_left (.clk(clk), .rst(rst), .in(left), .out(AUDIO2));
    dac_dsm dac_right (.clk(clk), .rst(rst), .in(right), .out(AUDIO1));
endmodule
