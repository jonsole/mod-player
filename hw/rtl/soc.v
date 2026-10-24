// The system on chip: PicoRV32 soft CPU, firmware block RAM and peripherals
// on a simple bus.
//
// Address map (see firmware/hw.h):
//   0x0000_0000  firmware RAM (block RAM)
//   0x2000_0000  serial port: +0 data (write: send a byte), +4 status (bit 0: busy)
//   0x2000_0200  board I/O: +0 LEDs (write), +4 DIP switches (read)
module soc #(
    parameter CLK_HZ      = 50000000,
    parameter BAUD        = 1000000,
    parameter RAM_WORDS   = 8192,           // 32 KB
    parameter RAM_BITS    = 13,
    parameter FIRMWARE    = "build/fw/firmware"
) (
    input        clk,
    input        rst,
    output       uart_tx,
    input        uart_rx,
    output reg [7:0] leds,
    input  [3:0] switches
);
    // ---- CPU ----
    wire        mem_valid, mem_instr;
    reg         mem_ready;
    wire [31:0] mem_addr, mem_wdata;
    wire [3:0]  mem_wstrb;
    reg  [31:0] mem_rdata;

    picorv32 #(
        .ENABLE_MUL(1),
        .ENABLE_DIV(1),
        .BARREL_SHIFTER(1),
        .COMPRESSED_ISA(0),
        .ENABLE_COUNTERS(1),
        .PROGADDR_RESET(32'h0000_0000),
        .STACKADDR(RAM_WORDS * 4)
    ) cpu (
        .clk(clk), .resetn(!rst),
        .mem_valid(mem_valid), .mem_instr(mem_instr), .mem_ready(mem_ready),
        .mem_addr(mem_addr), .mem_wdata(mem_wdata), .mem_wstrb(mem_wstrb),
        .mem_rdata(mem_rdata)
    );

    // ---- address decode ----
    wire sel_ram  = mem_addr[31:28] == 4'h0;
    wire sel_io   = mem_addr[31:28] == 4'h2;
    wire sel_uart = sel_io && mem_addr[15:8] == 8'h00;
    wire sel_gpio = sel_io && mem_addr[15:8] == 8'h02;

    // ---- firmware RAM ----
    wire [31:0] ram_rdata;
    reg         ram_busy;
    bram #(.WORDS(RAM_WORDS), .ADDR_BITS(RAM_BITS), .INIT_PREFIX(FIRMWARE)) ram (
        .clk(clk), .addr(mem_addr[RAM_BITS+1:2]),
        .we(mem_valid && sel_ram && !ram_busy ? mem_wstrb : 4'b0000),
        .wdata(mem_wdata), .rdata(ram_rdata)
    );

    // ---- serial port ----
    wire uart_busy;
    uart_tx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) utx (
        .clk(clk), .rst(rst),
        .start(mem_valid && !mem_ready && sel_uart && mem_addr[3:2] == 2'd0 && mem_wstrb != 0),
        .data(mem_wdata[7:0]), .busy(uart_busy), .tx(uart_tx)
    );

    // ---- bus responses ----
    always @(posedge clk) begin
        mem_ready <= 1'b0;
        if (rst) begin
            ram_busy <= 1'b0;
            leds <= 8'd0;
        end else if (mem_valid && !mem_ready) begin
            if (sel_ram) begin
                // block RAM data is ready one cycle after the address
                ram_busy <= !ram_busy;
                if (ram_busy) begin
                    mem_rdata <= ram_rdata;
                    mem_ready <= 1'b1;
                end
            end else if (sel_uart) begin
                mem_rdata <= {31'd0, uart_busy};
                mem_ready <= 1'b1;
            end else if (sel_gpio) begin
                if (mem_wstrb != 0 && mem_addr[3:2] == 2'd0)
                    leds <= mem_wdata[7:0];
                mem_rdata <= {28'd0, switches};
                mem_ready <= 1'b1;
            end else begin
                mem_rdata <= 32'd0;         // unmapped: reads 0
                mem_ready <= 1'b1;
            end
        end
    end

    wire unused = uart_rx | mem_instr;
endmodule
