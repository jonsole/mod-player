// Serial transmitter, 8N1. Pulse `start` with `data` when not `busy`.
module uart_tx #(
    parameter CLK_HZ = 50000000,
    parameter BAUD   = 1000000
) (
    input            clk,
    input            rst,
    input            start,
    input      [7:0] data,
    output           busy,
    output reg       tx
);
    localparam DIV = CLK_HZ / BAUD;
    reg [15:0] count;
    reg [3:0]  bits;        // bits left to send, including start and stop
    reg [8:0]  shift;       // {stop, data}; start bit goes out first

    assign busy = bits != 0;

    always @(posedge clk) begin
        if (rst) begin
            tx <= 1'b1;
            bits <= 0;
            count <= 0;
            shift <= 9'h1FF;
        end else if (!busy) begin
            if (start) begin
                tx <= 1'b0;                 // start bit
                shift <= {1'b1, data};
                bits <= 4'd10;
                count <= DIV[15:0] - 1'b1;
            end
        end else if (count != 0) begin
            count <= count - 1'b1;
        end else begin
            bits <= bits - 1'b1;
            if (bits != 4'd1) begin
                tx <= shift[0];
                shift <= {1'b1, shift[8:1]};
                count <= DIV[15:0] - 1'b1;
            end
        end
    end
endmodule
