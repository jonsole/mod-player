// HDMI Resampler: Paula's frames (one per colour clock) in, 48 kHz out.
//
// Behaviour is defined by the C model (host/resampler.c, arithmetic in
// host/resampler.h), which this must match output for output (ADR-0005,
// ADR-0010):
//  * CIC: four integrators per channel, updated last stage first from the
//    others' old values, so each is one 32-bit adder. Every 16th frame the
//    last integrator goes through four combs, one per clock.
//  * FIR: 128 taps at the two table phases either side of the output's
//    position, both channels at once (four multipliers, one tap per clock),
//    then a 12-step shift-and-add interpolation between the two results.
//
// One output takes about 150 clocks; outputs are at least 64 frames apart,
// so frames may arrive as often as every third clock. If a new output falls
// due while the last is still being computed, it is skipped and `overrun`
// sets (it never should; tests check it).
//
// Memories: the coefficient table (resampler_coefs.hex, COEFS) is a
// 2048 x 18 dual-port ROM in two RAMB16s; the history is distributed RAM.
// No RAMB8s: ISE 14.7's bitgen mishandles them.
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).

module resampler #(
    parameter COEFS = "resampler_coefs.hex"
) (
    input wire               clk,
    input wire               rst,
    input wire               in_valid,
    input wire signed [15:0] in_l,
    input wire signed [15:0] in_r,
    output reg               out_valid,
    output reg signed [23:0] out_l,
    output reg signed [23:0] out_r,
    output reg               overrun
);
    // ---- CIC ----------------------------------------------------------------
    reg [31:0] il0, il1, il2, il3, ir0, ir1, ir2, ir3;   // integrators
    reg [31:0] cl0, cl1, cl2, cl3, cr0, cr1, cr2, cr3;   // comb delays, rotating
    reg [31:0] vl, vr;                                   // value moving through the combs
    reg [3:0]  count;                                    // frames into the group
    reg [2:0]  cs;                                       // comb step: 0 idle, 1..4 combs, 5 store

    // ---- history and output position ---------------------------------------
    (* ram_style = "distributed" *) reg [35:0] hist [0:255];  // {left, right}, 18 bits each
    reg [7:0]  head;                   // latest intermediate sample
    reg [2:0]  gap;                    // intermediate samples to skip before the next output
    reg [4:0]  phase;                  // 1/32 sample
    reg [12:0] frac;                   // 1/4800 of a phase step

    wire [13:0] frac_step = {1'b0, frac} + 14'd3779;
    wire        frac_carry = frac_step >= 14'd4800;
    wire [24:0] frac_w = {12'd0, frac} * 25'd3495;   // weight = frac * 3495 >> 12
    wire [5:0]  phase_step = {1'b0, phase} + 6'd19 + {5'd0, frac_carry};

    // ---- coefficient ROM ------------------------------------------------------
    reg [17:0] coefs [0:2047];
    initial $readmemh(COEFS, coefs);
    wire [10:0] addr_a, addr_b;        // from the issue stage, below
    reg  [17:0] qa, qb;
    always @(posedge clk) begin
        qa <= coefs[addr_a];
        qb <= coefs[addr_b];
    end

    // ---- FIR ----------------------------------------------------------------
    reg         busy;
    reg  [7:0]  base;                  // head at the start of this output
    reg  [4:0]  fp;                    // this output's phase
    reg  [11:0] fw;                    // this output's interpolation weight
    reg  [7:0]  j;                     // tap being issued
    reg         issuing;
    reg  [2:0]  pipe;                  // valid bits: data, product, (accumulate)
    reg         za1, zb1;              // coefficient is h[0] or h[4096] (zero)
    reg  [35:0] hd1;                   // history sample for the tap
    reg  signed [35:0] pal, pbl, par, pbr;   // products
    reg  signed [39:0] aal, abl, aar, abr;   // accumulators
    reg  [3:0]  mix;                   // shift-and-add step, 0 = not mixing
    reg  signed [53:0] ml, mr;         // mixed results
    reg  signed [53:0] dl, dr;         // (acc_b - acc_a) << step
    reg  [11:0] wbits;

    wire [11:0] na = {j[6:0], 5'd0} + {7'd0, fp};    // 0..4095
    wire [12:0] nb = {1'b0, na} + 13'd1;             // 1..4096
    wire signed [17:0] ha = za1 ? 18'sd0 : qa;
    wire signed [17:0] hb = zb1 ? 18'sd0 : qb;
    wire signed [17:0] hl = hd1[35:18];
    wire signed [17:0] hr = hd1[17:0];

    function [10:0] fold;               // table index of h[n], n = 1..4095
        input [12:0] n;
        fold = (n <= 13'd2048) ? n - 1'b1 : 13'd4095 - n;
    endfunction

    function signed [23:0] sat24;
        input signed [53:0] m;
        reg signed [53:0] t;
        begin
            t = (m + 54'sd4194304) >>> 23;
            sat24 = (t > 54'sd8388607) ? 24'sd8388607 :
                    (t < -54'sd8388608) ? -24'sd8388608 : t[23:0];
        end
    endfunction

    assign addr_a = fold({1'b0, na});
    assign addr_b = fold(nb);

    always @(posedge clk) begin
        // issue stage: ROM and history read now, data next clock
        za1 <= na == 12'd0;
        zb1 <= nb == 13'd4096;
        hd1 <= hist[base - j];
        // product stage
        pal <= hl * ha;
        pbl <= hl * hb;
        par <= hr * ha;
        pbr <= hr * hb;
    end

    always @(posedge clk) begin
        out_valid <= 1'b0;
        if (rst) begin
            il0 <= 0; il1 <= 0; il2 <= 0; il3 <= 0; ir0 <= 0; ir1 <= 0; ir2 <= 0; ir3 <= 0;
            cl0 <= 0; cl1 <= 0; cl2 <= 0; cl3 <= 0; cr0 <= 0; cr1 <= 0; cr2 <= 0; cr3 <= 0;
            vl <= 0; vr <= 0; count <= 0; cs <= 0;
            head <= 0; gap <= 0; phase <= 0; frac <= 0;
            busy <= 0; issuing <= 0; pipe <= 0; mix <= 0; j <= 0;
            base <= 0; fp <= 0; fw <= 0;
            overrun <= 0;
        end else begin
            // ---- CIC integrators ----
            if (in_valid) begin
                il3 <= il3 + il2; il2 <= il2 + il1; il1 <= il1 + il0; il0 <= il0 + {{16{in_l[15]}}, in_l};
                ir3 <= ir3 + ir2; ir2 <= ir2 + ir1; ir1 <= ir1 + ir0; ir0 <= ir0 + {{16{in_r[15]}}, in_r};
                if (count == 4'd15) begin
                    vl <= il3 + il2;
                    vr <= ir3 + ir2;
                    cs <= 3'd1;
                end
                count <= count + 1'b1;
            end

            // ---- combs, one per clock ----
            if (cs >= 3'd1 && cs <= 3'd4) begin
                vl <= vl - cl0;
                {cl0, cl1, cl2, cl3} <= {cl1, cl2, cl3, vl};
                vr <= vr - cr0;
                {cr0, cr1, cr2, cr3} <= {cr1, cr2, cr3, vr};
                cs <= cs + 1'b1;
            end

            // ---- store the intermediate sample; start an output if due ----
            if (cs == 3'd5) begin
                cs <= 3'd0;
                hist[head + 1'b1] <= {vl[31:14], vr[31:14]};
                head <= head + 1'b1;
                if (gap != 0) begin
                    gap <= gap - 1'b1;
                end else begin
                    if (busy) begin
                        overrun <= 1'b1;
                    end else begin
                        busy <= 1'b1;
                        issuing <= 1'b1;
                        j <= 0;
                        base <= head + 1'b1;
                        fp <= phase;
                        fw <= frac_w[23:12];
                        aal <= 0; abl <= 0; aar <= 0; abr <= 0;
                    end
                    frac <= frac_carry ? frac_step[12:0] - 13'd4800 : frac_step[12:0];
                    phase <= phase_step[4:0];
                    gap <= phase_step[5] ? 3'd4 : 3'd3;
                end
            end

            // ---- FIR: issue 128 taps, then drain the pipeline ----
            pipe <= {pipe[1:0], issuing};
            if (issuing) begin
                j <= j + 1'b1;
                if (j == 8'd127) issuing <= 1'b0;
            end
            if (pipe[1]) begin          // products of a tap are ready
                aal <= aal + pal; abl <= abl + pbl;
                aar <= aar + par; abr <= abr + pbr;
            end
            if (busy && !issuing && pipe == 3'b100) begin   // last tap accumulated
                ml <= {{2{aal[39]}}, aal, 12'd0};
                mr <= {{2{aar[39]}}, aar, 12'd0};
                dl <= {{14{abl[39]}}, abl} - {{14{aal[39]}}, aal};
                dr <= {{14{abr[39]}}, abr} - {{14{aar[39]}}, aar};
                wbits <= fw;
                mix <= 4'd1;
            end
            if (mix != 0) begin
                if (wbits[0]) begin
                    ml <= ml + dl;
                    mr <= mr + dr;
                end
                dl <= dl <<< 1;
                dr <= dr <<< 1;
                wbits <= wbits >> 1;
                mix <= mix + 1'b1;
                if (mix == 4'd13) begin
                    // the 12th step's sum landed last clock; wbits is now 0
                    mix <= 0;
                    busy <= 0;
                    out_valid <= 1'b1;
                    out_l <= sat24(ml);
                    out_r <= sat24(mr);
                end
            end
        end
    end
endmodule
