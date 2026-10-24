// Firmware RAM: 32-bit words with byte enables, one-cycle read latency.
// Built as four byte-wide memories so Xilinx XST maps each to block RAM;
// INIT_PREFIX names four $readmemh files, <prefix>0.hex .. <prefix>3.hex,
// one per byte lane (lane 0 = bits 7:0).
module bram #(
    parameter WORDS = 8192,
    parameter ADDR_BITS = 13,
    parameter INIT_PREFIX = "build/fw/firmware"
) (
    input                 clk,
    input [ADDR_BITS-1:0] addr,     // word address
    input [3:0]           we,       // byte enables
    input [31:0]          wdata,
    output [31:0]         rdata
);
    reg [7:0] lane0 [0:WORDS-1];
    reg [7:0] lane1 [0:WORDS-1];
    reg [7:0] lane2 [0:WORDS-1];
    reg [7:0] lane3 [0:WORDS-1];
    reg [7:0] q0, q1, q2, q3;

    initial begin
        $readmemh({INIT_PREFIX, "0.hex"}, lane0);
        $readmemh({INIT_PREFIX, "1.hex"}, lane1);
        $readmemh({INIT_PREFIX, "2.hex"}, lane2);
        $readmemh({INIT_PREFIX, "3.hex"}, lane3);
    end

    always @(posedge clk) begin
        if (we[0]) lane0[addr] <= wdata[7:0];
        q0 <= lane0[addr];
    end
    always @(posedge clk) begin
        if (we[1]) lane1[addr] <= wdata[15:8];
        q1 <= lane1[addr];
    end
    always @(posedge clk) begin
        if (we[2]) lane2[addr] <= wdata[23:16];
        q2 <= lane2[addr];
    end
    always @(posedge clk) begin
        if (we[3]) lane3[addr] <= wdata[31:24];
        q3 <= lane3[addr];
    end
    assign rdata = {q3, q2, q1, q0};
endmodule
