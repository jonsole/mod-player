// One scope Voice with the shared engine to itself, for scope_capture_tb.cpp:
// the same two modules hw/rtl/scopes.v puts four Voices on, with the other
// three never asking for anything.
`default_nettype none
module scope_one (
    input wire               clk,
    input wire               rst,
    input wire               audio_valid,
    input wire signed [15:0] v,
    input wire               start,
    input wire               clear,
    output wire [1:0]        phase,
    output wire              wen,
    output wire [8:0]        widx,
    output wire [6:0]        wdat
);
    wire        req, neg, c_wen, e_wen, finished;
    wire [3:0]  take, done;
    wire [10:0] gap, off, q0, dq, off_new;
    wire [8:0]  j, j_new, c_widx, e_widx;
    wire [6:0]  c_wdat, e_wdat;
    wire [1:0]  e_wsel;

    scope_capture cap (
        .clk(clk), .rst(rst), .audio_valid(audio_valid), .v(v), .start(start), .clear(clear),
        .phase(phase), .wen(c_wen), .widx(c_widx), .wdat(c_wdat),
        .req(req), .s_gap(gap), .s_off(off), .s_q0(q0), .s_neg(neg), .s_dq(dq),
        .take(take[0]), .j_out(j), .done(done[0]), .off_new(off_new), .j_new(j_new), .finished(finished)
    );
    scope_engine eng (
        .clk(clk), .rst(rst), .req({3'b000, req}),
        .g_gap({33'd0, gap}), .g_off({33'd0, off}), .g_q0({33'd0, q0}), .g_neg({3'b000, neg}),
        .g_dq({33'd0, dq}), .g_j({27'd0, j}),
        .take(take), .done(done), .off_out(off_new), .j_out(j_new), .finished(finished),
        .wen(e_wen), .widx(e_widx), .wdat(e_wdat), .wsel(e_wsel)
    );
    assign wen = c_wen | e_wen;
    assign widx = c_wen ? c_widx : e_widx;
    assign wdat = c_wen ? c_wdat : e_wdat;
endmodule
`default_nettype wire
