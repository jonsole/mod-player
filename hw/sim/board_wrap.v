// Wraps the board top level for simulation: resolves the SDRAM data bus
// between the FPGA and the SDRAM model, and exposes the FPGA's output
// enable to the model's checks.
module board_wrap (
    input         CLK50,
    output [7:0]  LEDS,
    input  [4:1]  SW,
    input         FTDI_D0,
    output        FTDI_D1,
    output        AUDIO1,
    output        AUDIO2,
    output        SDRAM_CLK, SDRAM_CKE, SDRAM_CSn, SDRAM_RASn, SDRAM_CASn, SDRAM_WEn,
    output [1:0]  SDRAM_BA,
    output [12:0] SDRAM_A,
    output        SDRAM_LDQM, SDRAM_UDQM,
    output [15:0] dq_from_fpga,
    output        dq_fpga_drives,
    input  [15:0] dq_from_sdram,
    input         dq_sdram_drives
);
    wire [15:0] SDRAM_D;
    assign SDRAM_D = dq_sdram_drives ? dq_from_sdram : 16'bz;
    assign dq_from_fpga = SDRAM_D;
    assign dq_fpga_drives = board.soc.sd_dq_oe;

    top #(.FW0({`FIRMWARE, "0.hex"}), .FW1({`FIRMWARE, "1.hex"}),
          .FW2({`FIRMWARE, "2.hex"}), .FW3({`FIRMWARE, "3.hex"})) board (
        .CLK50(CLK50), .LEDS(LEDS), .SW(SW), .FTDI_D0(FTDI_D0), .FTDI_D1(FTDI_D1),
        .AUDIO1(AUDIO1), .AUDIO2(AUDIO2),
        .SDRAM_CLK(SDRAM_CLK), .SDRAM_CKE(SDRAM_CKE), .SDRAM_CSn(SDRAM_CSn),
        .SDRAM_RASn(SDRAM_RASn), .SDRAM_CASn(SDRAM_CASn), .SDRAM_WEn(SDRAM_WEn),
        .SDRAM_BA(SDRAM_BA), .SDRAM_A(SDRAM_A), .SDRAM_LDQM(SDRAM_LDQM), .SDRAM_UDQM(SDRAM_UDQM),
        .SDRAM_D(SDRAM_D)
    );
endmodule
