// TMDS video encoder (DVI 1.0 section 3.2.2): 8 bits to 10, transition
// minimised and DC balanced. One character per clock, registered. While
// `video` is low the running disparity resets to 0, as in a Control Period,
// and `other` (a control, guard band or TERC4 character) goes out instead.
// Bit 0 of the output is sent first.
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
module tmds_encoder (
    input wire       clk,
    input wire       video,
    input wire [7:0] d,
    input wire [9:0] other,
    output reg [9:0] q
);
    function [3:0] ones8;
        input [7:0] v;
        ones8 = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
    endfunction

    wire [3:0] n1d  = ones8(d);
    wire       use_xnor = (n1d > 4'd4) || (n1d == 4'd4 && !d[0]);
    wire [8:0] qm;
    assign qm[0] = d[0];
    genvar i;
    generate
        for (i = 1; i < 8; i = i + 1) begin : stage
            assign qm[i] = use_xnor ? ~(qm[i-1] ^ d[i]) : (qm[i-1] ^ d[i]);
        end
    endgenerate
    assign qm[8] = !use_xnor;

    wire [3:0] n1q = ones8(qm[7:0]);
    wire signed [7:0] diff = {3'b000, n1q, 1'b0} - 8'd8;   // ones minus zeros in qm[7:0]

    reg signed [7:0] cnt;               // running disparity: ones minus zeros sent

    always @(posedge clk) begin
        if (!video) begin
            cnt <= 0;
            q <= other;
        end else if (cnt == 0 || diff == 0) begin
            q <= {~qm[8], qm[8], qm[8] ? qm[7:0] : ~qm[7:0]};
            cnt <= qm[8] ? cnt + diff : cnt - diff;
        end else if ((cnt > 0 && diff > 0) || (cnt < 0 && diff < 0)) begin
            q <= {1'b1, qm[8], ~qm[7:0]};
            cnt <= cnt + (qm[8] ? 8'sd2 : 8'sd0) - diff;
        end else begin
            q <= {1'b0, qm[8], qm[7:0]};
            cnt <= cnt - (qm[8] ? 8'sd0 : 8'sd2) + diff;
        end
    end
endmodule
