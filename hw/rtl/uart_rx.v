// Serial receiver, 8N1, with a small byte queue.
// `pop` takes the oldest byte (`data`); `avail` says one is waiting.
// `overrun` is set (until cleared) if a byte arrives with the queue full.
module uart_rx #(
    parameter CLK_HZ = 50000000,
    parameter BAUD   = 1000000,
    parameter DEPTH_BITS = 6            // 64 bytes
) (
    input            clk,
    input            rst,
    input            rx,
    input            pop,
    input            clear_overrun,
    output     [7:0] data,
    output           avail,
    output reg       overrun
);
    localparam DIV = CLK_HZ / BAUD;
    reg [1:0]  sync;                    // two flip-flops against metastability
    reg [15:0] count;
    reg [3:0]  bits;                    // 0: idle; 1..8 data; 9 stop
    reg [7:0]  shift;

    reg [7:0]  fifo [0:(1 << DEPTH_BITS) - 1];
    reg [DEPTH_BITS-1:0] wp, rp;
    reg [DEPTH_BITS:0]   level;
    assign data  = fifo[rp];
    assign avail = level != 0;

    wire line = sync[1];
    wire push = bits == 4'd9 && count == 0 && line;       // good stop bit
    wire full = level == (1 << DEPTH_BITS);

    always @(posedge clk) begin
        if (push && !full)
            fifo[wp] <= shift;
    end

    always @(posedge clk) begin
        sync <= {sync[0], rx};
        if (rst) begin
            sync <= 2'b11;
            bits <= 0;
            count <= 0;
            wp <= 0; rp <= 0; level <= 0;
            overrun <= 1'b0;
        end else begin
            if (bits == 0) begin
                if (!line) begin                // start bit: sample mid-bit from here on
                    bits <= 4'd1;
                    count <= DIV[15:0] + DIV[15:0] / 2 - 1'b1;
                end
            end else if (count != 0) begin
                count <= count - 1'b1;
            end else if (bits <= 4'd8) begin
                shift <= {line, shift[7:1]};
                bits <= bits + 1'b1;
                count <= DIV[15:0] - 1'b1;
            end else begin
                bits <= 0;                      // stop bit (bad ones are dropped)
            end

            if (push && full) overrun <= 1'b1;
            if (clear_overrun) overrun <= 1'b0;
            if (push && !full) wp <= wp + 1'b1;
            if (pop && avail) rp <= rp + 1'b1;
            level <= level + {{DEPTH_BITS{1'b0}}, push && !full} - {{DEPTH_BITS{1'b0}}, pop && avail};
        end
    end
endmodule
