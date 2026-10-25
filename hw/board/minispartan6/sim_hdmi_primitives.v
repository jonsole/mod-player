// Simulation stand-ins for the Spartan-6 primitives hdmi_phy.v uses, so the
// HDMI build passes the Verilator lint and Yosys checks here. ISE uses its
// own. The clock models only pass the clock through: they check wiring, not
// frequencies (hw/sim/hdmi_chain_tb.cpp runs the real clock ratio).
module DCM_SP #(parameter CLKIN_PERIOD = 20.0, parameter CLKFX_MULTIPLY = 2,
                parameter CLKFX_DIVIDE = 1, parameter CLK_FEEDBACK = "1X",
                parameter STARTUP_WAIT = "FALSE") (
    input CLKIN, input CLKFB, input RST, input PSEN, input PSINCDEC, input PSCLK, input DSSEN,
    output CLKFX, output LOCKED, output [7:0] STATUS,
    output CLK0, output CLK90, output CLK180, output CLK270, output CLK2X, output CLK2X180,
    output CLKDV, output CLKFX180, output PSDONE
);
    assign CLKFX = CLKIN;
    assign LOCKED = !RST;
    assign STATUS = 8'd0;
    assign {CLK0, CLK90, CLK180, CLK270, CLK2X, CLK2X180, CLKDV, CLKFX180, PSDONE} = 9'd0;
    wire unused = CLKFB | PSEN | PSINCDEC | PSCLK | DSSEN;
endmodule

module PLL_BASE #(parameter CLKIN_PERIOD = 10.0, parameter DIVCLK_DIVIDE = 1,
                  parameter CLKFBOUT_MULT = 1, parameter CLKOUT0_DIVIDE = 1,
                  parameter CLKOUT1_DIVIDE = 1, parameter CLK_FEEDBACK = "CLKFBOUT",
                  parameter COMPENSATION = "SYSTEM_SYNCHRONOUS") (
    input CLKIN, input CLKFBIN, input RST,
    output CLKFBOUT, output CLKOUT0, output CLKOUT1, output CLKOUT2, output CLKOUT3,
    output CLKOUT4, output CLKOUT5, output LOCKED
);
    assign CLKFBOUT = CLKIN;
    assign CLKOUT0 = CLKIN;
    assign CLKOUT1 = CLKIN;
    assign {CLKOUT2, CLKOUT3, CLKOUT4, CLKOUT5} = 4'd0;
    assign LOCKED = !RST;
    wire unused = CLKFBIN;
endmodule

module OBUFDS (input I, output O, output OB);
    assign O = I;
    assign OB = !I;
endmodule
