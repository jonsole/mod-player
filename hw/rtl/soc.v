// The system on chip: PicoRV32 soft CPU, firmware block RAM and peripherals
// on a simple bus.
//
// Address map (see firmware/hw.h):
//   0x0000_0000  firmware RAM (block RAM)
//   0x1000_0000  SDRAM, 32 MB
//   0x2000_0000  serial port: +0 data (write: send a byte; read: take a
//                received byte), +4 status (bit 0: sending, bit 1: byte
//                waiting, bit 2: receive overrun; write bit 2 to clear)
//   0x2000_0100  Paula (firmware/replayer/paula_port.h)
//   0x2000_0200  board I/O: +0 LEDs (write), +4 DIP switches (read, 1 = on)
//
// The SDRAM is shared: Paula's Sample fetches take priority over the CPU.
`default_nettype none
module soc #(
    parameter CLK_HZ      = 50000000,
    parameter BAUD        = 1000000,
    parameter RAM_WORDS   = 8192,           // 32 KB
    parameter RAM_BITS    = 13,
    parameter FW0         = "firmware0.hex",  // block-RAM files, one per byte lane
    parameter FW1         = "firmware1.hex",
    parameter FW2         = "firmware2.hex",
    parameter FW3         = "firmware3.hex",
    parameter PAULA_FREE_RUN = 0                 // simulation only: Paula as fast as it can
) (
    input wire                   clk,
    input wire                   rst,
    output wire                  uart_tx,
    input wire                   uart_rx,
    output reg           [7:0]   leds,
    input wire           [3:0]   switches,
    // SDRAM (the board top adds the inverted clock and the DQ tristate)
    output wire                  sd_cke,
    output wire                  sd_cs_n,
    output wire                  sd_ras_n,
    output wire                  sd_cas_n,
    output wire                  sd_we_n,
    output wire          [1:0]   sd_ba,
    output wire          [12:0]  sd_a,
    output wire          [1:0]   sd_dqm,
    output wire          [15:0]  sd_dq_o,
    output wire                  sd_dq_oe,
    input wire           [15:0]  sd_dq_i,
    // audio: one stereo frame per colour clock
    output wire                  audio_valid,
    output wire signed   [15:0]  audio_l,
    output wire signed   [15:0]  audio_r,
    // Paula register writes, for testbenches (leave unconnected)
    output wire                  dbg_paula_we,
    output wire          [6:0]   dbg_paula_addr,
    output wire          [31:0]  dbg_paula_wdata
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
        .mem_rdata(mem_rdata),
        // unused: coprocessor interface and interrupts
        .pcpi_wr(1'b0), .pcpi_rd(32'd0), .pcpi_wait(1'b0), .pcpi_ready(1'b0), .irq(32'd0)
    );

    // ---- address decode ----
    wire sel_ram  = mem_addr[31:28] == 4'h0;
    wire sel_sdram = mem_addr[31:28] == 4'h1;
    wire sel_io   = mem_addr[31:28] == 4'h2;
    wire sel_uart = sel_io && mem_addr[15:8] == 8'h00;
    wire sel_gpio = sel_io && mem_addr[15:8] == 8'h02;
    wire sel_paula = sel_io && mem_addr[15:8] == 8'h01;

    // ---- firmware RAM ----
    wire [31:0] ram_rdata;
    reg         ram_busy;
    bram #(.WORDS(RAM_WORDS), .ADDR_BITS(RAM_BITS), .INIT0(FW0), .INIT1(FW1), .INIT2(FW2), .INIT3(FW3)) ram (
        .clk(clk), .addr(mem_addr[RAM_BITS+1:2]),
        .we(mem_valid && sel_ram && !ram_busy ? mem_wstrb : 4'b0000),
        .wdata(mem_wdata), .rdata(ram_rdata)
    );

    wire        sd_accept, sd_ack;
    wire [31:0] sd_rdata;

    // ---- Paula ----
    wire        p_req, p_we, p_ack;
    wire [15:0] p_rdata;
    wire [24:1] p_addr;
    wire [15:0] p_wdata;
    wire [1:0]  p_be;
    wire [31:0] p_rdata_reg;
    wire        paula_we = mem_valid && !mem_ready && sel_paula && mem_wstrb != 0;
    paula #(.CLK_HZ(CLK_HZ), .FREE_RUN(PAULA_FREE_RUN)) paula (
        .clk(clk), .rst(rst),
        .reg_we(paula_we), .reg_addr(mem_addr[6:0]), .reg_wdata(mem_wdata),
        .reg_rdata(p_rdata_reg),
        .mem_req(p_req), .mem_addr(p_addr), .mem_we(p_we), .mem_wdata(p_wdata), .mem_be(p_be),
        .mem_ack(p_ack), .mem_rdata(p_rdata),
        .out_valid(audio_valid), .out_l(audio_l), .out_r(audio_r)
    );
    assign dbg_paula_we = paula_we;
    assign dbg_paula_addr = mem_addr[6:0];
    assign dbg_paula_wdata = mem_wdata;

    // ---- SDRAM, shared: Paula first ----
    wire        cpu_req = mem_valid && sel_sdram && !mem_ready;
    reg         owner_paula, paula_half;   // who the access in flight belongs to
    wire        pick_paula = p_req;
    always @(posedge clk)
        if (sd_accept) begin
            owner_paula <= pick_paula;
            paula_half <= p_addr[1];
        end
    assign      p_ack = sd_ack && owner_paula;
    assign      p_rdata = paula_half ? sd_rdata[31:16] : sd_rdata[15:0];
    wire        cpu_ack = sd_ack && !owner_paula;
    sdram_ctrl sdram (
        .clk(clk), .rst(rst),
        .req(pick_paula || cpu_req),
        .we(pick_paula ? p_we : mem_wstrb != 0),
        .addr(pick_paula ? p_addr[24:2] : mem_addr[24:2]),
        .wdata(pick_paula ? {p_wdata, p_wdata} : mem_wdata),
        .wstrb(pick_paula ? (p_addr[1] ? {p_be, 2'b00} : {2'b00, p_be}) : mem_wstrb),
        .accept(sd_accept), .ack(sd_ack), .rdata(sd_rdata),
        .sd_cke(sd_cke), .sd_cs_n(sd_cs_n), .sd_ras_n(sd_ras_n), .sd_cas_n(sd_cas_n),
        .sd_we_n(sd_we_n), .sd_ba(sd_ba), .sd_a(sd_a), .sd_dqm(sd_dqm),
        .sd_dq_o(sd_dq_o), .sd_dq_oe(sd_dq_oe), .sd_dq_i(sd_dq_i)
    );

    // ---- serial port ----
    wire uart_busy;
    uart_tx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) utx (
        .clk(clk), .rst(rst),
        .start(mem_valid && !mem_ready && sel_uart && mem_addr[3:2] == 2'd0 && mem_wstrb != 0),
        .data(mem_wdata[7:0]), .busy(uart_busy), .tx(uart_tx)
    );

    wire       rx_avail, rx_overrun;
    wire [7:0] rx_data;
    wire       uart_read = mem_valid && !mem_ready && sel_uart && mem_wstrb == 0;
    uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) urx (
        .clk(clk), .rst(rst), .rx(uart_rx),
        .pop(uart_read && mem_addr[3:2] == 2'd0),
        .clear_overrun(mem_valid && !mem_ready && sel_uart && mem_addr[3:2] == 2'd1 &&
                       mem_wstrb != 0 && mem_wdata[2]),
        .data(rx_data), .avail(rx_avail), .overrun(rx_overrun)
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
            end else if (sel_sdram) begin
                if (cpu_ack) begin
                    mem_rdata <= sd_rdata;
                    mem_ready <= 1'b1;
                end
            end else if (sel_uart) begin
                mem_rdata <= mem_addr[3:2] == 2'd0 ? {24'd0, rx_data}
                                                   : {29'd0, rx_overrun, rx_avail, uart_busy};
                mem_ready <= 1'b1;
            end else if (sel_paula) begin
                mem_rdata <= p_rdata_reg;
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

    wire unused = mem_instr;
endmodule
`default_nettype wire
