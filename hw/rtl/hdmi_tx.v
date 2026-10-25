// HDMI transmitter: 640x480 at 60 Hz (CEA-861 VIC 1, 25.2 MHz pixel clock)
// with 48 kHz stereo audio (ADR-0010). Runs entirely on the pixel clock and
// produces one 10-bit character per TMDS channel per clock (bit 0 first);
// the board's serialiser sends them.
//
// Timing (one line = 800 clocks, one frame = 525 lines):
//   x 0..639 active video (lines 0..479), 656..751 HSYNC, lines 490..491
//   VSYNC (both negative; VSYNC edges at x = 656, with HSYNC's leading edge).
//   Each line has one Data Island in the blanking:
//     640..651 control (12), 644..651 its preamble
//     652..653 leading guard band, 654..685 packet 0, 686..717 packet 1,
//     718..719 trailing guard band
//   and, when the next line has video, 790..797 video preamble and
//   798..799 video guard band. Control periods are 12 and 70+ clocks.
//
// Packets (HDMI 1.3 section 5.3): packet 0 is an Audio Sample packet when
// samples are waiting (1 or 2 per line), else an Audio Clock Regeneration
// packet if one is due, else null. Packet 1 is the ACR packet if still
// due, else the AVI InfoFrame or Audio InfoFrame once per frame, else null.
// ACR (N = 6144, CTS = 25200) falls due every 25 200 clocks: 1 kHz, which is
// 128 fs / N exactly.
//
// Audio: `audio_tick` pulses every 525 clocks (25.2 MHz / 48 kHz exactly);
// the sample on audio_l/audio_r is taken in the same clock. Samples are
// 24-bit two's complement, consumer IEC 60958 framing: channel status says
// PCM, copying permitted, 48 kHz, 24-bit, channels 1 (left) and 2 (right).
//
// Channel 0 bit 3 in a Data Island is 0 on the island's first packet clock
// and 1 after it (our reading of HDMI 1.3 figure 5-3).
//
// Plain Verilog-2001 for Xilinx ISE (ADR-0004).

module hdmi_tx #(
    parameter [23:0] COLOUR = 24'h0055AA        // the plain screen, RGB
) (
    input wire               clk,               // pixel clock, 25.2 MHz
    input wire               rst,
    output reg               audio_tick,
    input wire signed [23:0] audio_l,
    input wire signed [23:0] audio_r,
    output wire [9:0]        tmds0,             // blue, HSYNC/VSYNC, packet headers
    output wire [9:0]        tmds1,             // green
    output wire [9:0]        tmds2              // red
);
    // ---- frame counters ------------------------------------------------------
    reg [9:0] x, y;
    wire      hs = !(x >= 10'd656 && x < 10'd752);
    wire      vs = !((y == 10'd490 && x >= 10'd656) || y == 10'd491 || (y == 10'd492 && x < 10'd656));
    wire      next_line_video = (y == 10'd524) || (y < 10'd479);
    wire      video_now   = x < 10'd640 && y < 10'd480;
    wire      vid_pre     = next_line_video && x >= 10'd790 && x < 10'd798;
    wire      vid_guard   = next_line_video && x >= 10'd798;
    wire      di_pre      = x >= 10'd644 && x < 10'd652;
    wire      di_guard    = (x >= 10'd652 && x < 10'd654) || (x >= 10'd718 && x < 10'd720);
    wire      di_packet   = x >= 10'd654 && x < 10'd718;
    wire [4:0] pc         = x[4:0] - 5'd14;        // packet clock: 654 = 0 (mod 32)
    wire      load        = x == 10'd653 || x == 10'd685;

    // ---- audio: a sample every 525 clocks, queued for the next packet ----------
    reg [9:0]  tick_count;
    reg [7:0]  frame_no;                          // IEC 60958 frame in the 192-frame block
    reg [50:0] q0, q1, q2, q3;                    // {B, C_R, C_L, R[23:0], L[23:0]}
    reg [2:0]  q_count;

    function cs_bit;                              // channel status bit n
        input [7:0] n;
        input       right;
        cs_bit = n == 8'd2 ||                     // copying permitted
                 (n == 8'd20 && !right) || (n == 8'd21 && right) ||   // channel 1 / 2
                 n == 8'd25 ||                    // 48 kHz
                 n == 8'd32 || n == 8'd33 || n == 8'd35;              // 24-bit words
    endfunction

    wire [50:0] new_sample = {frame_no == 8'd0, cs_bit(frame_no, 1'b1), cs_bit(frame_no, 1'b0),
                              audio_r, audio_l};

    // ---- ACR and InfoFrame scheduling --------------------------------------------
    reg [14:0] acr_count;
    reg        acr_due, avi_due, aif_due;
    wire       acr_send = load && acr_due && !(x == 10'd653 && q_count != 0);
    // ACR subpacket {SB6..SB0}: SB1..3 CTS = 25200 (0x06270), SB4..6 N = 6144 (0x01800)
    localparam [55:0] ACR_SUB = {8'h00, 8'h18, 8'h00, 8'h70, 8'h62, 8'h00, 8'h00};

    // ---- the packet being sent ---------------------------------------------------
    reg [23:0] hdr;
    reg [55:0] sb0, sb1, sb2, sb3;
    reg [7:0]  ecc_h, ecc0, ecc1, ecc2, ecc3;

    function [7:0] ecc_next;                       // BCH ECC, G(x) = 1 + x^6 + x^7 + x^8
        input [7:0] e;
        input       b;
        ecc_next = (e >> 1) ^ ((e[0] ^ b) ? 8'b10000011 : 8'd0);
    endfunction

    // The audio sample subpacket for queue entry e (zero if not present).
    function [55:0] sample_sub;
        input [50:0] e;
        input        present;
        reg pl, pr;
        begin
            pl = ^{e[48], e[23:0]};                // even parity over sample, V, U, C
            pr = ^{e[49], e[47:24]};
            sample_sub = present ? {pr, e[49], 1'b0, 1'b0, pl, e[48], 1'b0, 1'b0, e[47:24], e[23:0]} : 56'd0;
        end
    endfunction

    // InfoFrames: PB0 is the checksum, so the bytes of the frame sum to 0.
    // AVI (type 0x82, version 2, 13 bytes): RGB, no bars or scan info,
    // 4:3 picture, active format as picture, VIC 1.
    localparam [7:0] AVI_PB1 = 8'h00, AVI_PB2 = 8'h18, AVI_PB3 = 8'h00, AVI_PB4 = 8'd1;
    localparam [7:0] AVI_SUM = 8'h82 + 8'h02 + 8'h0D + AVI_PB1 + AVI_PB2 + AVI_PB3 + AVI_PB4;
    // Audio InfoFrame (type 0x84, version 1, 10 bytes): 2 channels, coding
    // type and sample size and rate from the stream, speakers FL/FR.
    localparam [7:0] AIF_PB1 = 8'h01;
    localparam [7:0] AIF_SUM = 8'h84 + 8'h01 + 8'h0A + AIF_PB1;

    // ---- per-clock character data (stage 1) ---------------------------------------
    reg [2:0] mode;                               // 0 control, 1 video, 2 video guard, 3 island, 4 island guard
    reg [5:0] ctl;                                // {CTL3, CTL2, CTL1, CTL0, VSYNC, HSYNC}
    reg [11:0] terc;                              // TERC4 data, 4 bits per channel

    wire [5:0] bit_lo = {pc, 1'b0};
    wire [5:0] bit_hi = {pc, 1'b1};
    wire [63:0] bch0 = {ecc0, sb0}, bch1 = {ecc1, sb1}, bch2 = {ecc2, sb2}, bch3 = {ecc3, sb3};
    wire [31:0] bch4 = {ecc_h, hdr};

    always @(posedge clk) begin
        if (rst) begin
            x <= 0; y <= 0;
            tick_count <= 0; frame_no <= 0; q_count <= 0;
            q0 <= 0; q1 <= 0; q2 <= 0; q3 <= 0;
            acr_count <= 0; acr_due <= 0; avi_due <= 0; aif_due <= 0;
            hdr <= 0; sb0 <= 0; sb1 <= 0; sb2 <= 0; sb3 <= 0;
            ecc_h <= 0; ecc0 <= 0; ecc1 <= 0; ecc2 <= 0; ecc3 <= 0;
            mode <= 0; ctl <= 0; terc <= 0;
            audio_tick <= 0;
        end else begin
            // counters
            x <= (x == 10'd799) ? 10'd0 : x + 1'b1;
            if (x == 10'd799) y <= (y == 10'd524) ? 10'd0 : y + 1'b1;

            // 48 kHz: audio_tick is high in the clock the sample is taken
            tick_count <= (tick_count == 10'd524) ? 10'd0 : tick_count + 1'b1;
            audio_tick <= tick_count == 10'd523;

            // ACR every 25 200 clocks; InfoFrames once a frame
            acr_count <= (acr_count == 15'd25199) ? 15'd0 : acr_count + 1'b1;
            if (x == 10'd0 && y == 10'd0) begin
                avi_due <= 1'b1;
                aif_due <= 1'b1;
            end

            // load the next packet; the queue takes the new sample either way
            if (load) begin
                ecc_h <= 0; ecc0 <= 0; ecc1 <= 0; ecc2 <= 0; ecc3 <= 0;
                if (x == 10'd653 && q_count != 0) begin
                    hdr <= {{q_count >= 3'd4 && q3[50], q_count >= 3'd3 && q2[50],
                             q_count >= 3'd2 && q1[50], q0[50], 4'b0000},   // HB2: B.3..0, flat
                            {3'b000, 1'b0, q_count >= 3'd4, q_count >= 3'd3, q_count >= 3'd2, 1'b1}, // HB1
                            8'h02};
                    sb0 <= sample_sub(q0, 1'b1);
                    sb1 <= sample_sub(q1, q_count >= 3'd2);
                    sb2 <= sample_sub(q2, q_count >= 3'd3);
                    sb3 <= sample_sub(q3, q_count >= 3'd4);
                    q_count <= audio_tick ? 3'd1 : 3'd0;
                    q0 <= new_sample;
                end else begin
                    if (acr_due) begin
                        hdr <= 24'h000001;
                        sb0 <= ACR_SUB; sb1 <= ACR_SUB; sb2 <= ACR_SUB; sb3 <= ACR_SUB;
                    end else if (x == 10'd685 && avi_due) begin
                        avi_due <= 1'b0;
                        hdr <= {8'h0D, 8'h02, 8'h82};
                        sb0 <= {8'h00, 8'h00, AVI_PB4, AVI_PB3, AVI_PB2, AVI_PB1, 8'h00 - AVI_SUM};
                        sb1 <= 56'd0; sb2 <= 56'd0; sb3 <= 56'd0;
                    end else if (x == 10'd685 && aif_due) begin
                        aif_due <= 1'b0;
                        hdr <= {8'h0A, 8'h01, 8'h84};
                        sb0 <= {8'h00, 8'h00, 8'h00, 8'h00, 8'h00, AIF_PB1, 8'h00 - AIF_SUM};
                        sb1 <= 56'd0; sb2 <= 56'd0; sb3 <= 56'd0;
                    end else begin
                        hdr <= 24'd0;
                        sb0 <= 56'd0; sb1 <= 56'd0; sb2 <= 56'd0; sb3 <= 56'd0;
                    end
                    if (audio_tick) begin
                        case (q_count)
                            3'd0: q0 <= new_sample;
                            3'd1: q1 <= new_sample;
                            3'd2: q2 <= new_sample;
                            default: q3 <= new_sample;
                        endcase
                        if (q_count != 3'd4) q_count <= q_count + 1'b1;
                    end
                end
            end else if (audio_tick) begin
                case (q_count)
                    3'd0: q0 <= new_sample;
                    3'd1: q1 <= new_sample;
                    3'd2: q2 <= new_sample;
                    default: q3 <= new_sample;
                endcase
                if (q_count != 3'd4) q_count <= q_count + 1'b1;
            end
            if (audio_tick) frame_no <= (frame_no == 8'd191) ? 8'd0 : frame_no + 1'b1;
            if (acr_count == 15'd25199) acr_due <= 1'b1;
            else if (acr_send) acr_due <= 1'b0;

            // ECC over the bits as they go out
            if (di_packet) begin
                if (pc < 5'd24) ecc_h <= ecc_next(ecc_h, hdr[pc]);
                if (pc < 5'd28) begin
                    ecc0 <= ecc_next(ecc_next(ecc0, sb0[bit_lo]), sb0[bit_hi]);
                    ecc1 <= ecc_next(ecc_next(ecc1, sb1[bit_lo]), sb1[bit_hi]);
                    ecc2 <= ecc_next(ecc_next(ecc2, sb2[bit_lo]), sb2[bit_hi]);
                    ecc3 <= ecc_next(ecc_next(ecc3, sb3[bit_lo]), sb3[bit_hi]);
                end
            end

            // this clock's characters
            mode <= video_now ? 3'd1 : vid_guard ? 3'd2 : di_guard ? 3'd4 : di_packet ? 3'd3 : 3'd0;
            ctl <= {1'b0, di_pre, 1'b0, vid_pre || di_pre, vs, hs};
            terc <= {bch3[bit_hi], bch2[bit_hi], bch1[bit_hi], bch0[bit_hi],
                     bch3[bit_lo], bch2[bit_lo], bch1[bit_lo], bch0[bit_lo],
                     !(x == 10'd654), bch4[pc], vs, hs};
        end
    end

    // ---- stage 2: encode ----------------------------------------------------------
    function [9:0] terc4;
        input [3:0] d;
        case (d)
            4'h0: terc4 = 10'b1010011100; 4'h1: terc4 = 10'b1001100011;
            4'h2: terc4 = 10'b1011100100; 4'h3: terc4 = 10'b1011100010;
            4'h4: terc4 = 10'b0101110001; 4'h5: terc4 = 10'b0100011110;
            4'h6: terc4 = 10'b0110001110; 4'h7: terc4 = 10'b0100111100;
            4'h8: terc4 = 10'b1011001100; 4'h9: terc4 = 10'b0100111001;
            4'hA: terc4 = 10'b0110011100; 4'hB: terc4 = 10'b1011000110;
            4'hC: terc4 = 10'b1010001110; 4'hD: terc4 = 10'b1001110001;
            4'hE: terc4 = 10'b0101100011; default: terc4 = 10'b1011000011;
        endcase
    endfunction

    function [9:0] control;
        input [1:0] c;
        case (c)
            2'b00: control = 10'b1101010100; 2'b01: control = 10'b0010101011;
            2'b10: control = 10'b0101010100; default: control = 10'b1010101011;
        endcase
    endfunction

    function [9:0] other;                          // a channel's character outside video
        input [2:0] m;
        input [1:0] c;
        input [3:0] t;
        input [1:0] ch;
        case (m)
            3'd2: other = (ch == 2'd1) ? 10'b0100110011 : 10'b1011001100;   // video guard
            3'd3: other = terc4(t);
            3'd4: other = (ch == 2'd0) ? terc4({2'b11, c}) : 10'b0100110011; // island guard
            default: other = control(c);
        endcase
    endfunction

    wire video = mode == 3'd1;
    tmds_encoder enc0 (.clk(clk), .video(video), .d(COLOUR[7:0]), .other(other(mode, ctl[1:0], terc[3:0], 2'd0)), .q(tmds0));
    tmds_encoder enc1 (.clk(clk), .video(video), .d(COLOUR[15:8]), .other(other(mode, ctl[3:2], terc[7:4], 2'd1)), .q(tmds1));
    tmds_encoder enc2 (.clk(clk), .video(video), .d(COLOUR[23:16]), .other(other(mode, ctl[5:4], terc[11:8], 2'd2)), .q(tmds2));
endmodule
