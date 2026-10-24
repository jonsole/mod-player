// SDRAM controller for the board's 32 MB, 16-bit SDRAM (16M x 16: 4 banks,
// 8192 rows, 512 columns) at 50 MHz.
//
// Each access is one 32-bit word: ACTIVE, then READ or WRITE with auto
// precharge and a burst of 2 (low half first). Byte enables become DQM.
// Rows are closed after every access, so there is no page bookkeeping.
// The SDRAM clock is the controller clock inverted (done at the pin), so
// commands and write data are sampled half a cycle after they leave here.
// Read data then arrives about 2-16 ns after one of our rising edges (the
// chip's 6 ns access time plus pin delays), so it is captured on our
// FALLING edge, in the middle of that window, 3.5 clocks after the READ,
// and used at the next rising edge.
//
// Host port: hold req (with we/addr/wdata/wstrb) until ack, which pulses
// for one clock with rdata for reads.
//
// Timing (cycles of 20 ns): tRCD 2, tRC 6, tRP 1, tWR 2, tRFC 5, refresh
// every 370 (7.4 us); 10100 cycles (202 us) of power-up wait.
module sdram_ctrl #(
    parameter INIT_CYCLES    = 10100,
    parameter REFRESH_CYCLES = 370
) (
    input             clk,
    input             rst,
    // host
    input             req,
    input             we,
    input      [24:2] addr,      // word address
    input      [31:0] wdata,
    input      [3:0]  wstrb,
    output            accept,    // req is being taken this clock
    output reg        ack,
    output reg [31:0] rdata,
    // SDRAM pins (registered, so they can live in the I/O blocks)
    output reg        sd_cke,
    output reg        sd_cs_n,
    output reg        sd_ras_n,
    output reg        sd_cas_n,
    output reg        sd_we_n,
    output reg [1:0]  sd_ba,
    output reg [12:0] sd_a,
    output reg [1:0]  sd_dqm,
    output reg [15:0] sd_dq_o,
    output reg        sd_dq_oe,
    input      [15:0] sd_dq_i
);
    // commands: {ras_n, cas_n, we_n}
    localparam C_NOP = 3'b111, C_ACT = 3'b011, C_READ = 3'b101, C_WRITE = 3'b100,
               C_PRE = 3'b010, C_REF = 3'b001, C_MRS = 3'b000;
    localparam [12:0] MODE = 13'h021;           // CL 2, burst 2, sequential

    localparam S_INIT = 4'd0, S_PRE = 4'd1, S_INIT_REF = 4'd2, S_MRS = 4'd3, S_IDLE = 4'd4,
               S_RCD = 4'd6, S_RW = 4'd7, S_READ = 4'd8, S_WRITE = 4'd9,
               S_WAIT = 4'd10;
    // Power-up values: FPGA registers start at these at configuration, before
    // reset, so the chip sees "deselected, NOP" rather than a stray command.
    initial begin
        sd_cke = 1'b1; sd_cs_n = 1'b1; sd_ras_n = 1'b1; sd_cas_n = 1'b1; sd_we_n = 1'b1;
        sd_dq_oe = 1'b0; sd_dqm = 2'b11;
    end

    reg  [3:0]  state, after_wait;
    reg  [13:0] count;
    reg  [3:0]  init_refs;
    reg  [9:0]  refresh_timer;
    reg         refresh_due;
    reg  [2:0]  step;

    reg         l_we;
    reg  [9:2]  l_col;                          // column part of the latched address
    reg  [15:0] dq_fall;                        // read data, captured mid-window

    always @(negedge clk)
        dq_fall <= sd_dq_i;
    reg  [31:0] l_wdata;
    reg  [3:0]  l_wstrb;

    assign accept = state == S_IDLE && !refresh_due && req;

    task command;
        input [2:0] c;
        begin
            {sd_ras_n, sd_cas_n, sd_we_n} <= c;
        end
    endtask

    always @(posedge clk) begin
        ack <= 1'b0;
        command(C_NOP);
        if (rst) begin
            state <= S_INIT;
            count <= 0;
            init_refs <= 0;
            sd_cke <= 1'b1;
            sd_cs_n <= 1'b1;
            sd_dq_oe <= 1'b0;
            sd_dqm <= 2'b11;
            sd_ba <= 0;
            sd_a <= 0;
            refresh_timer <= 0;
            refresh_due <= 1'b0;
        end else begin
            if (state > S_MRS) begin
                if (refresh_timer == REFRESH_CYCLES - 1) begin
                    refresh_timer <= 0;
                    refresh_due <= 1'b1;
                end else begin
                    refresh_timer <= refresh_timer + 1'b1;
                end
            end

            case (state)
            // ---- power-up: wait, PRECHARGE ALL, 8 REFRESHes, MODE REGISTER ----
            S_INIT: begin
                sd_cs_n <= 1'b0;
                count <= count + 1'b1;
                if (count == INIT_CYCLES) state <= S_PRE;
            end
            S_PRE: begin
                command(C_PRE);
                sd_a <= 13'h0400;               // all banks
                count <= 2; after_wait <= S_INIT_REF; state <= S_WAIT;
            end
            S_INIT_REF: begin
                command(C_REF);
                init_refs <= init_refs + 1'b1;
                count <= 5;
                after_wait <= init_refs == 4'd7 ? S_MRS : S_INIT_REF;
                state <= S_WAIT;
            end
            S_MRS: begin
                command(C_MRS);
                sd_ba <= 2'b00;
                sd_a <= MODE;
                count <= 3; after_wait <= S_IDLE; state <= S_WAIT;
            end

            // ---- normal operation ----
            S_IDLE: begin
                if (refresh_due) begin
                    refresh_due <= 1'b0;
                    command(C_REF);
                    count <= 5; after_wait <= S_IDLE; state <= S_WAIT;
                end else if (req) begin
                    l_we <= we; l_col <= addr[9:2]; l_wdata <= wdata; l_wstrb <= wstrb;
                    command(C_ACT);
                    sd_ba <= addr[24:23];
                    sd_a <= addr[22:10];        // row
                    state <= S_RCD;
                end
            end
            S_RCD: state <= S_RW;               // tRCD: one NOP between ACTIVE and READ/WRITE
            S_RW: begin
                sd_a <= {4'b0010, l_col, 1'b0};   // A10: auto precharge; even column
                step <= 0;
                if (l_we) begin
                    command(C_WRITE);
                    sd_dq_o <= l_wdata[15:0];
                    sd_dqm <= ~l_wstrb[1:0];
                    sd_dq_oe <= 1'b1;
                    state <= S_WRITE;
                end else begin
                    command(C_READ);
                    sd_dqm <= 2'b00;
                    state <= S_READ;
                end
            end
            S_WRITE: begin                      // second beat, then let precharge finish
                sd_dq_o <= l_wdata[31:16];
                sd_dqm <= ~l_wstrb[3:2];
                ack <= 1'b1;
                count <= 3; after_wait <= S_IDLE; state <= S_WAIT;
            end
            S_READ: begin                       // captured 3.5 and 4.5 clocks after READ
                step <= step + 1'b1;
                if (step == 3'd3) rdata[15:0] <= dq_fall;
                if (step == 3'd4) begin
                    rdata[31:16] <= dq_fall;
                    ack <= 1'b1;
                    count <= 1; after_wait <= S_IDLE; state <= S_WAIT;
                end
            end
            S_WAIT: begin
                sd_dq_oe <= 1'b0;               // write data (if any) is done
                if (count <= 1) state <= after_wait;
                count <= count - 1'b1;
            end
            default: state <= S_IDLE;
            endcase
        end
    end
endmodule
