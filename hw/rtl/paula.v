// Paula: four Amiga-style sample-playback voices and their output stage.
//
// Behaviour is defined by the C model (host/paula_model.c), which this
// must match frame for frame (ADR-0005); register meanings are in
// firmware/replayer/paula_port.h.
//
// Timing (ADR-0009):
//  * Writes to the voice, DMACON, LED and OPTIONS registers are queued and
//    applied together at the next tick boundary. Paula counts the tick
//    itself: TICK_LEN colour clocks per tick, reloaded at each boundary.
//  * The colour-clock engine waits whenever it needs Sample data from
//    memory, then catches up, so output frames are exact whatever the
//    memory timing. One frame per processed colour clock: out_valid pulses.
//
// Memory: 16-bit words at even byte addresses; [7:0] is the byte at the
// address, [15:8] the next byte. Hold mem_req until mem_ack. Writes
// (mem_we, with byte enables) only happen for INVERT, at a tick boundary.
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).

module paula #(
    parameter CLK_HZ   = 50000000,
    parameter CCK_HZ   = 3546895,
    parameter FREE_RUN = 0          // simulation: process colour clocks back to back
) (
    input             clk,
    input             rst,
    // register bus (32-bit words; reg_addr is the byte offset)
    input             reg_we,
    input      [6:0]  reg_addr,
    input      [31:0] reg_wdata,
    output reg [31:0] reg_rdata,
    // Sample memory
    output reg        mem_req,
    output reg [24:1] mem_addr,
    output reg        mem_we,
    output reg [15:0] mem_wdata,
    output reg [1:0]  mem_be,
    input             mem_ack,
    input      [15:0] mem_rdata,
    // audio: one stereo frame per colour clock
    output reg        out_valid,
    output reg signed [15:0] out_l,
    output reg signed [15:0] out_r
);

    // ---- register map --------------------------------------------------
    localparam R_DMACON  = 7'h40, R_LED = 7'h44, R_OPTIONS = 7'h48, R_INVERT = 7'h4C;
    localparam R_TICKLEN = 7'h50, R_STATUS = 7'h54, R_CONTROL = 7'h58;

    // filter coefficients (see paula_model.h)
    localparam [17:0] A500_K = 18'd511, LED_F = 18'd359, LED_DAMP = 18'd99297;

    // ---- write queue -----------------------------------------------------
    // entry = {word index (reg_addr[6:2]), value}
    reg  [36:0] fifo [0:127];
    reg  [36:0] fifo_q;
    reg  [6:0]  fifo_wp, fifo_rp;
    reg  [7:0]  fifo_count;
    wire        queued = reg_addr < R_TICKLEN;

    // ---- control and status ----------------------------------------------
    reg         running, tick_flag, overflow, behind;
    reg  [31:0] tick_len, tick_left;

    // ---- colour-clock pacing -----------------------------------------------
    reg  [31:0] phase;
    reg  [7:0]  pending;               // colour clocks due but not yet processed
    wire        cck_ready = (FREE_RUN != 0) || (pending != 0);
    wire        cck_due   = (FREE_RUN == 0) && running && (phase + CCK_HZ >= CLK_HZ);

    // ---- voice state -------------------------------------------------------
    reg  [24:0] v_ptr [0:3];           // written by the Replayer
    reg  [15:0] v_len [0:3];
    reg  [16:0] v_per [0:3];           // 113..65536
    reg  [6:0]  v_vol [0:3];           // 0..64
    reg  [3:0]  v_on;
    reg  [24:0] v_loc [0:3];           // next byte to fetch
    reg  [15:0] v_left [0:3];          // words left before the loop reload
    reg  [3:0]  v_just_started;
    reg  [7:0]  v_dat0 [0:3], v_dat1 [0:3];
    reg  [1:0]  v_bytes [0:3];
    reg  [16:0] v_per_left [0:3];
    reg  signed [14:0] v_out [0:3];    // sample x volume

    reg  [2:0]  options;
    reg         led;

    // ---- engine ------------------------------------------------------------
    // Per colour clock: S_MIX (mix the current samples, count down every
    // voice's period), S_STEP (play the next byte for voices whose period ran
    // out, fetching a word when one is used up), S_POST (blend, A500 filter,
    // first LED filter step), then S_LED2/S_LED3 if the LED filter is on.
    localparam S_IDLE = 4'd0, S_COMMIT = 4'd1, S_COMMIT_WAIT = 4'd2, S_APPLY = 4'd3,
               S_START = 4'd4, S_MIX = 4'd5, S_STEP = 4'd6, S_FETCH = 4'd7,
               S_POST = 4'd8, S_LED2 = 4'd9, S_LED3 = 4'd10, S_INV_RD = 4'd11,
               S_INV_WR = 4'd12;
    reg  [3:0]  state, fetch_ret;
    reg  [7:0]  drain_left;
    reg  [3:0]  start_mask, step_mask;
    reg  [1:0]  vi;                    // voice waiting for a fetch
    reg         inv_hi;                // INVERT: which byte of the word

    reg  signed [15:0] mix_l, mix_r;
    reg  signed [31:0] y_l, y_r;       // Q15
    reg  signed [31:0] a500_l, a500_r, low_l, low_r, band_l, band_r, high_l, high_r;
    reg  signed [31:0] damp_l, damp_r;

    /* verilator lint_off UNUSEDSIGNAL */
    /* verilator lint_off BLKSEQ */
    function signed [31:0] mul16;      // (a * k) >> 16, as in the C model
        input signed [31:0] a;
        input [17:0] k;
        reg signed [50:0] p;
        begin
            p = a * $signed({1'b0, k});
            mul16 = p[47:16];
        end
    endfunction
    /* verilator lint_on BLKSEQ */

    function signed [15:0] voice_sample; // a voice's contribution to the mix
        input on;
        input signed [14:0] out;
        voice_sample = on ? {out[14], out} : 16'sd0;
    endfunction

    function [1:0] lowest;             // lowest set bit of a non-zero mask
        input [3:0] m;
        lowest = m[0] ? 2'd0 : m[1] ? 2'd1 : m[2] ? 2'd2 : 2'd3;
    endfunction
    /* verilator lint_on UNUSEDSIGNAL */

    wire [4:0]  q_idx   = fifo_q[36:32];
    /* verilator lint_off UNUSEDSIGNAL */
    wire [31:0] q_val   = fifo_q[31:0];  // registers use the low bits they need
    /* verilator lint_on UNUSEDSIGNAL */
    wire [15:0] q_per16 = q_val[15:0];
    wire [6:0]  q_vol7  = q_val[6:0];

    // output stage, computed from the latched mix
    wire signed [17:0] l18 = {{2{mix_l[15]}}, mix_l}, r18 = {{2{mix_r[15]}}, mix_r};
    wire signed [17:0] blend_l = (l18 + l18 + l18 + r18) >>> 2;
    wire signed [17:0] blend_r = (l18 + r18 + r18 + r18) >>> 2;
    wire signed [31:0] x_l = options[0] ? {{14{blend_l[17]}}, blend_l} : {{16{mix_l[15]}}, mix_l};
    wire signed [31:0] x_r = options[0] ? {{14{blend_r[17]}}, blend_r} : {{16{mix_r[15]}}, mix_r};
    wire signed [31:0] y0_l = x_l <<< 15, y0_r = x_r <<< 15;
    wire signed [31:0] a500_l_next = a500_l + mul16(y0_l - a500_l, A500_K);
    wire signed [31:0] a500_r_next = a500_r + mul16(y0_r - a500_r, A500_K);
    wire signed [31:0] y1_l = options[1] ? a500_l_next : y0_l;
    wire signed [31:0] y1_r = options[1] ? a500_r_next : y0_r;
    wire               led_on = led && !options[2];
    wire signed [15:0] y1_l_out = y1_l[30:15], y1_r_out = y1_r[30:15];   // >> 15
    wire signed [15:0] low_l_out = low_l[30:15], low_r_out = low_r[30:15];

    // what to do after finishing a colour clock (tick_left already counted down)
    wire [31:0] tick_left_after = tick_left - 1'b1;
    wire        boundary_next   = tick_left_after == 0;

    // the voice S_STEP works on, and its fetch address
    wire [1:0]  sv       = lowest(step_mask);
    wire        reload   = !v_just_started[sv] && v_left[sv] == 16'd1;
    wire [24:0] fetch_at = reload ? v_ptr[sv] : v_loc[sv];

    always @(*) begin
        case (reg_addr)
            R_TICKLEN: reg_rdata = tick_len;
            R_STATUS:  reg_rdata = {24'd0, fifo_count == 8'd128, 3'd0, running, behind, overflow, tick_flag};
            default:   reg_rdata = 32'd0;
        endcase
    end

    always @(posedge clk)
        fifo_q <= fifo[fifo_rp];

    always @(posedge clk) begin
        if (reg_we && queued && fifo_count != 8'd128)
            fifo[fifo_wp] <= {reg_addr[6:2], reg_wdata};
    end

    integer i;
    always @(posedge clk) begin
        out_valid <= 1'b0;
        if (rst) begin
            state <= S_IDLE;
            fifo_wp <= 0; fifo_rp <= 0; fifo_count <= 0;
            running <= 0; tick_flag <= 0; overflow <= 0; behind <= 0;
            tick_len <= 32'd70940; tick_left <= 0;
            phase <= 0; pending <= 0;
            mem_req <= 0; mem_we <= 0; mem_be <= 2'b11; mem_wdata <= 0;
            v_on <= 0; v_just_started <= 0;
            options <= 0; led <= 0;
            a500_l <= 0; a500_r <= 0; low_l <= 0; low_r <= 0; band_l <= 0; band_r <= 0;
            out_l <= 0; out_r <= 0;
            for (i = 0; i < 4; i = i + 1) begin
                v_ptr[i] <= 0; v_len[i] <= 0; v_per[i] <= 17'd65536; v_vol[i] <= 0;
                v_loc[i] <= 0; v_left[i] <= 0; v_dat0[i] <= 0; v_dat1[i] <= 0;
                v_bytes[i] <= 0; v_per_left[i] <= 0; v_out[i] <= 0;
            end
        end else begin
            // ---- bus: queue and immediate registers ----
            if (reg_we && queued) begin
                if (fifo_count == 8'd128)
                    overflow <= 1'b1;
                else
                    fifo_wp <= fifo_wp + 1'b1;
            end
            fifo_count <= fifo_count + {7'd0, reg_we && queued && fifo_count != 8'd128}
                                     - {7'd0, state == S_APPLY};
            if (reg_we && reg_addr == R_TICKLEN)
                tick_len <= reg_wdata;
            if (reg_we && reg_addr == R_STATUS) begin
                if (reg_wdata[0]) tick_flag <= 1'b0;
                if (reg_wdata[1]) overflow <= 1'b0;
                if (reg_wdata[2]) behind <= 1'b0;
            end

            // ---- colour-clock pacing: S_MIX consumes one due colour clock ----
            if (FREE_RUN == 0) begin
                phase <= (phase + CCK_HZ >= CLK_HZ) ? phase + CCK_HZ - CLK_HZ : phase + CCK_HZ;
                if (cck_due && state != S_MIX && pending == 8'hFF)
                    behind <= 1'b1;
                else
                    pending <= pending + {7'd0, cck_due} - {7'd0, state == S_MIX};
            end

            case (state)
            S_IDLE: begin
                if (running && tick_left == 0) begin
                    drain_left <= fifo_count;   // only writes made before the boundary
                    state <= S_COMMIT;
                end else if (running && cck_ready) begin
                    state <= S_MIX;
                end
            end

            // ---- apply the queued writes ----
            S_COMMIT: begin
                if (drain_left == 0) begin
                    tick_left <= tick_len;
                    tick_flag <= 1'b1;
                    state <= S_IDLE;
                end else begin
                    state <= S_COMMIT_WAIT;     // fifo_q catches up with fifo_rp
                end
            end
            S_COMMIT_WAIT: state <= S_APPLY;
            S_APPLY: begin
                fifo_rp <= fifo_rp + 1'b1;
                drain_left <= drain_left - 1'b1;
                state <= S_COMMIT;
                if (q_idx < 5'd16) begin
                    case (q_idx[1:0])
                        2'd0: v_ptr[q_idx[3:2]] <= {q_val[24:1], 1'b0};
                        2'd1: v_len[q_idx[3:2]] <= q_val[15:0];
                        2'd2: v_per[q_idx[3:2]] <= q_per16 == 0 ? 17'd65536 :
                                                   q_per16 < 16'd113 ? 17'd113 : {1'b0, q_per16};
                        default: v_vol[q_idx[3:2]] <= q_vol7 > 7'd64 ? 7'd64 : q_vol7;
                    endcase
                end else if (q_idx == R_DMACON[6:2]) begin
                    if (q_val[15]) begin
                        start_mask <= q_val[3:0];
                        state <= S_START;
                    end else begin
                        v_on <= v_on & ~q_val[3:0];
                    end
                end else if (q_idx == R_LED[6:2]) begin
                    led <= q_val[0];
                end else if (q_idx == R_OPTIONS[6:2]) begin
                    options <= q_val[2:0];
                end else if (q_idx == R_INVERT[6:2]) begin
                    mem_addr <= q_val[24:1];
                    inv_hi <= q_val[0];
                    mem_req <= 1'b1;
                    state <= S_INV_RD;
                end
            end
            S_INV_RD: begin                     // read the word...
                if (mem_ack) begin
                    mem_wdata <= ~mem_rdata;
                    mem_be <= inv_hi ? 2'b10 : 2'b01;
                    mem_we <= 1'b1;             // mem_req stays high
                    state <= S_INV_WR;
                end
            end
            S_INV_WR: begin                     // ...and write back the flipped byte
                if (mem_ack) begin
                    mem_req <= 1'b0;
                    mem_we <= 1'b0;
                    mem_be <= 2'b11;
                    state <= S_COMMIT;
                end
            end
            S_START: begin                      // start each voice in start_mask, in order
                if (start_mask == 0) begin
                    state <= S_COMMIT;
                end else begin
                    vi <= lowest(start_mask);
                    start_mask[lowest(start_mask)] <= 1'b0;
                    v_left[lowest(start_mask)] <= v_len[lowest(start_mask)];
                    v_just_started[lowest(start_mask)] <= 1'b0;
                    v_per_left[lowest(start_mask)] <= v_per[lowest(start_mask)];
                    v_on[lowest(start_mask)] <= 1'b1;
                    // first word: straight from PTR
                    mem_addr <= v_ptr[lowest(start_mask)][24:1];
                    v_loc[lowest(start_mask)] <= v_ptr[lowest(start_mask)] + 25'd2;
                    mem_req <= 1'b1;
                    fetch_ret <= S_START;
                    state <= S_FETCH;
                end
            end

            // ---- one colour clock ----
            S_MIX: begin
                mix_l <= voice_sample(v_on[0], v_out[0]) + voice_sample(v_on[3], v_out[3]);
                mix_r <= voice_sample(v_on[1], v_out[1]) + voice_sample(v_on[2], v_out[2]);
                for (i = 0; i < 4; i = i + 1) begin
                    step_mask[i] <= v_on[i] && v_per_left[i] == 17'd1;
                    if (v_on[i])
                        v_per_left[i] <= v_per_left[i] == 17'd1 ? v_per[i] : v_per_left[i] - 1'b1;
                end
                state <= S_STEP;
            end
            S_STEP: begin
                if (step_mask == 0) begin
                    state <= S_POST;
                end else begin
                    step_mask[sv] <= 1'b0;
                    if (v_bytes[sv] != 0) begin  // next byte of the current word
                        v_out[sv] <= $signed(v_dat0[sv]) * $signed({1'b0, v_vol[sv]});
                        v_dat0[sv] <= v_dat1[sv];
                        v_bytes[sv] <= v_bytes[sv] - 1'b1;
                    end else begin               // word used up: fetch the next
                        if (reload) v_left[sv] <= v_len[sv];
                        else if (!v_just_started[sv]) v_left[sv] <= v_left[sv] - 1'b1;
                        v_just_started[sv] <= 1'b0;
                        mem_addr <= fetch_at[24:1];
                        v_loc[sv] <= fetch_at + 25'd2;
                        mem_req <= 1'b1;
                        vi <= sv;
                        fetch_ret <= S_STEP;
                        state <= S_FETCH;
                    end
                end
            end
            S_FETCH: begin                      // play the fetched word's first byte
                if (mem_ack) begin
                    mem_req <= 1'b0;
                    v_out[vi] <= $signed(mem_rdata[7:0]) * $signed({1'b0, v_vol[vi]});
                    v_dat0[vi] <= mem_rdata[15:8];
                    v_bytes[vi] <= 2'd1;
                    state <= fetch_ret;
                end
            end
            S_POST: begin
                if (options[1]) begin
                    a500_l <= a500_l_next;
                    a500_r <= a500_r_next;
                end
                y_l <= y1_l;
                y_r <= y1_r;
                if (led_on) begin               // LED step 1: low += band * f
                    low_l <= low_l + mul16(band_l, LED_F);
                    low_r <= low_r + mul16(band_r, LED_F);
                    damp_l <= mul16(band_l, LED_DAMP);
                    damp_r <= mul16(band_r, LED_DAMP);
                    state <= S_LED2;
                end else begin
                    out_l <= y1_l_out;
                    out_r <= y1_r_out;
                    out_valid <= 1'b1;
                    tick_left <= tick_left_after;
                    state <= (!boundary_next && cck_ready) ? S_MIX : S_IDLE;
                end
            end
            S_LED2: begin                       // high = y - low - band * damping
                high_l <= y_l - low_l - damp_l;
                high_r <= y_r - low_r - damp_r;
                state <= S_LED3;
            end
            S_LED3: begin                       // band += high * f; output low
                band_l <= band_l + mul16(high_l, LED_F);
                band_r <= band_r + mul16(high_r, LED_F);
                out_l <= low_l_out;
                out_r <= low_r_out;
                out_valid <= 1'b1;
                tick_left <= tick_left_after;
                state <= S_IDLE;
            end
            default: state <= S_IDLE;
            endcase

            // control register last, so it wins over the engine
            if (reg_we && reg_addr == R_CONTROL) begin
                running <= reg_wdata[0];
                tick_left <= 0;
                if (!reg_wdata[0]) pending <= 0;
                if (reg_wdata[1]) begin         // FLUSH: discard queued writes
                    fifo_rp <= fifo_wp;
                    fifo_count <= 0;
                    drain_left <= 0;
                end
            end
        end
    end
endmodule
