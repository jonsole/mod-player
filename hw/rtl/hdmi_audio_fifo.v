// Carries 48 kHz audio from the system clock to the HDMI pixel clock
// (ADR-0010). Both clocks come from the same 50 MHz oscillator, so samples
// arrive and leave at exactly the same average rate; the FIFO only absorbs
// Paula's jitter (it catches up after memory waits) and the Resampler's
// bunching.
//
// The pixel side waits until 16 samples are queued (0.33 ms) before taking
// any, so it starts with room either way; until then, and whenever the FIFO
// runs dry (Paula stopped), it hands out silence and waits to refill.
//
// A standard dual-clock FIFO: 32 entries of distributed RAM (no RAMB8s:
// ISE 14.7's bitgen mishandles them), Gray-coded pointers through two
// flip-flops each way. Timing constraints treat the crossing as false paths.
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).
module hdmi_audio_fifo (
    // system clock side
    input wire               wclk,
    input wire               wrst,
    input wire               w_valid,
    input wire signed [23:0] w_l,
    input wire signed [23:0] w_r,
    output reg               overflow,        // sticky: a sample was dropped
    // pixel clock side
    input wire               rclk,
    input wire               rrst,
    input wire               r_take,          // take a sample this clock (48 kHz)
    output wire signed [23:0] r_l,            // valid in the clock r_take is high
    output wire signed [23:0] r_r,
    output reg               primed,
    output reg               underflow        // sticky: ran dry after priming
);
    (* ram_style = "distributed" *) reg [47:0] mem [0:31];

    function [5:0] bin2gray;
        input [5:0] b;
        bin2gray = b ^ (b >> 1);
    endfunction
    function [5:0] gray2bin;
        input [5:0] g;
        gray2bin = {g[5], g[5] ^ g[4], g[5] ^ g[4] ^ g[3], g[5] ^ g[4] ^ g[3] ^ g[2],
                    g[5] ^ g[4] ^ g[3] ^ g[2] ^ g[1], g[5] ^ g[4] ^ g[3] ^ g[2] ^ g[1] ^ g[0]};
    endfunction

    // ---- write side ----
    reg [5:0] wbin, wgray;
    (* ASYNC_REG = "TRUE" *) reg [5:0] rgray_w1, rgray_w2;
    wire full = wgray == {~rgray_w2[5:4], rgray_w2[3:0]};
    always @(posedge wclk) begin
        if (wrst) begin
            wbin <= 0; wgray <= 0; rgray_w1 <= 0; rgray_w2 <= 0; overflow <= 0;
        end else begin
            rgray_w1 <= rgray;
            rgray_w2 <= rgray_w1;
            if (w_valid) begin
                if (full) begin
                    overflow <= 1'b1;
                end else begin
                    mem[wbin[4:0]] <= {w_r, w_l};
                    wbin <= wbin + 1'b1;
                    wgray <= bin2gray(wbin + 1'b1);
                end
            end
        end
    end

    // ---- read side ----
    reg [5:0] rbin, rgray;
    (* ASYNC_REG = "TRUE" *) reg [5:0] wgray_r1, wgray_r2;
    wire       empty = rgray == wgray_r2;
    wire [5:0] level = gray2bin(wgray_r2) - rbin;
    wire       give  = primed && !empty;
    wire [47:0] head = mem[rbin[4:0]];
    assign r_l = give ? head[23:0] : 24'd0;
    assign r_r = give ? head[47:24] : 24'd0;
    always @(posedge rclk) begin
        if (rrst) begin
            rbin <= 0; rgray <= 0; wgray_r1 <= 0; wgray_r2 <= 0; primed <= 0; underflow <= 0;
        end else begin
            wgray_r1 <= wgray;
            wgray_r2 <= wgray_r1;
            if (!primed) begin
                if (level >= 6'd16) primed <= 1'b1;
            end else if (r_take) begin
                if (empty) begin
                    primed <= 1'b0;
                    underflow <= 1'b1;
                end else begin
                    rbin <= rbin + 1'b1;
                    rgray <= bin2gray(rbin + 1'b1);
                end
            end
        end
    end
endmodule
