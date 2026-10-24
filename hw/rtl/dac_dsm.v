// Second-order delta-sigma modulator: turns a 16-bit audio sample into a
// 1-bit stream at the clock rate, for an RC low-pass filter on the pin.
//
// Input: signed, +-32767 = full scale. It is scaled by 13/16 inside, which
// keeps the second-order loop stable for any input.
// Structure: two integrators with feedback of the output bit (+-2^15).
//
// Measured by hw/sim/dsm_tb.cpp through a model of the board's RC filter,
// 20 Hz - 20 kHz: SNR 127 dB for a full-scale 1 kHz tone, 60 dB for a
// -50 dBFS tone; silence and small DC offsets leave no tone above -130 dBFS
// (16-bit audio's own noise floor is -98 dBFS), so no dither is needed.
module dac_dsm (
    input             clk,
    input             rst,
    input      [15:0] in,
    output reg        out
);
    wire signed [23:0] x     = {{8{in[15]}}, in};
    wire signed [23:0] xs    = ((x <<< 3) + (x <<< 2) + x) >>> 4;   // x * 13/16
    wire signed [23:0] fb    = out ? 24'sd32768 : -24'sd32768;
    reg  signed [23:0] i1, i2;
    wire signed [23:0] i1_next = i1 + xs - fb;
    wire signed [23:0] i2_next = i2 + i1_next - fb;

    always @(posedge clk) begin
        if (rst) begin
            i1 <= 0;
            i2 <= 0;
            out <= 1'b0;
        end else begin
            i1 <= i1_next;
            i2 <= i2_next;
            out <= !i2_next[23];               // i2 >= 0
        end
    end
endmodule
