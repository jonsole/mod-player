// SPI master for the board's SPI flash (issue #17): one byte at a time,
// mode 0 (SCK idles low; MOSI changes while SCK is low, both ends sample on
// the rising edge), most significant bit first, SCK = clk / 4 (12.5 MHz).
//
// Registers (soc.v maps them at 0x2000_0300):
//   +0 DATA   write: send this byte (and receive one); read: the byte
//             received by the last transfer
//   +4 CTRL   write bit 0: 1 = chip select active (pin low)
//             read: bit 0 busy, bit 1 chip select active
// Firmware waits for busy to clear before the next write or before
// changing chip select.
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
module spi_master (
    input wire        clk,
    input wire        rst,
    input wire        start,           // send `wdata`
    input wire [7:0]  wdata,
    input wire        set_cs,          // load chip select from `cs`
    input wire        cs,
    output reg  [7:0] rdata,
    output wire       busy,
    output wire       cs_active,
    // pins
    output reg        cs_n,
    output reg        sck,
    output reg        mosi,
    input wire        miso
);
    reg [7:0] shift;
    reg [3:0] bits_left;               // 0 = idle
    reg [1:0] phase;                   // 0,1: SCK low; 2,3: SCK high
    reg       miso_q;                  // MISO, registered at the pin

    assign busy = bits_left != 0;
    assign cs_active = !cs_n;

    always @(posedge clk) begin
        miso_q <= miso;
        if (rst) begin
            cs_n <= 1'b1;
            sck <= 1'b0;
            mosi <= 1'b0;
            bits_left <= 0;
            phase <= 0;
            rdata <= 8'd0;
        end else begin
            if (set_cs && !busy) cs_n <= !cs;
            if (start && !busy) begin
                shift <= wdata;
                mosi <= wdata[7];
                bits_left <= 4'd8;
                phase <= 0;
            end else if (busy) begin
                phase <= phase + 1'b1;
                case (phase)
                    2'd1: sck <= 1'b1;                       // rising edge: both ends sample
                    2'd2: shift <= {shift[6:0], miso_q};     // MISO as it was at the edge
                    2'd3: begin
                        sck <= 1'b0;
                        bits_left <= bits_left - 1'b1;
                        mosi <= shift[7];                    // next bit (shift moved last clock)
                        if (bits_left == 4'd1) rdata <= shift;
                    end
                    default: ;
                endcase
            end
        end
    end
endmodule
