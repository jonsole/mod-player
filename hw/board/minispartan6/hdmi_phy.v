// HDMI clocks and serialiser for the miniSpartan6+ (Spartan-6), ADR-0010.
//
// Clocks: 25.2 MHz pixel clock and 126 MHz (5x) bit clock from the 50 MHz
// system clock. A Spartan-6 PLL can't reach 25.2 MHz from 50 MHz directly
// (63/125 needs a divider whose phase-detector input would be under its
// 19 MHz minimum), so a DCM makes 42 MHz (x 21 / 25) first and the PLL
// takes that to 630 MHz (x 15), divided by 5 and 25. Both steps are exact,
// so the pixel clock is exactly 50 MHz x 63 / 125 and HDMI's 48 kHz is
// exactly the Resampler's (ADR-0010).
//
// Serialiser: each 10-bit character goes out bit 0 first, two bits per
// 126 MHz clock through an ODDR2 (252 Mbit/s per pair), to TMDS_33
// differential outputs. The pixel and 5x clocks come from the same PLL, so
// the 5x side takes a new character every fifth clock with no handshake.
//
// rst_pixel holds the pixel clock logic in reset until both DCM and PLL
// have locked.
//
// Pairs (Scarab's pin file, J3): 0 blue, 1 green, 2 red, 3 clock.
module hdmi_phy (
    input wire       clk,          // 50 MHz system clock (from its BUFG)
    output wire      clk_pixel,    // 25.2 MHz
    output reg       rst_pixel,
    input wire [9:0] tmds0,
    input wire [9:0] tmds1,
    input wire [9:0] tmds2,
    output wire [3:0] TMDS_P,
    output wire [3:0] TMDS_N
);
    // ---- clocks ----
    wire clk42_dcm, clk42, dcm_locked;
    wire [7:0] dcm_status;
    DCM_SP #(
        .CLKIN_PERIOD(20.0),
        .CLKFX_MULTIPLY(21),
        .CLKFX_DIVIDE(25),
        .CLK_FEEDBACK("NONE"),
        .STARTUP_WAIT("FALSE")
    ) dcm (
        .CLKIN(clk), .CLKFB(1'b0), .RST(1'b0), .PSEN(1'b0), .PSINCDEC(1'b0), .PSCLK(1'b0),
        .DSSEN(1'b0), .CLKFX(clk42_dcm), .LOCKED(dcm_locked), .STATUS(dcm_status),
        .CLK0(), .CLK90(), .CLK180(), .CLK270(), .CLK2X(), .CLK2X180(), .CLKDV(), .CLKFX180(), .PSDONE()
    );
    BUFG clk42_buf (.I(clk42_dcm), .O(clk42));

    wire pll_fb, clk_x5_pll, clk_pixel_pll, pll_locked;
    PLL_BASE #(
        .CLKIN_PERIOD(23.8095),              // 42 MHz
        .DIVCLK_DIVIDE(1),
        .CLKFBOUT_MULT(15),                  // VCO 630 MHz
        .CLKOUT0_DIVIDE(5),                  // 126 MHz
        .CLKOUT1_DIVIDE(25),                 // 25.2 MHz
        .CLK_FEEDBACK("CLKFBOUT"),
        .COMPENSATION("INTERNAL")
    ) pll (
        .CLKIN(clk42), .CLKFBIN(pll_fb), .CLKFBOUT(pll_fb), .RST(!dcm_locked),
        .CLKOUT0(clk_x5_pll), .CLKOUT1(clk_pixel_pll),
        .CLKOUT2(), .CLKOUT3(), .CLKOUT4(), .CLKOUT5(), .LOCKED(pll_locked)
    );
    wire clk_x5;
    BUFG clk_x5_buf (.I(clk_x5_pll), .O(clk_x5));
    BUFG clk_pixel_buf (.I(clk_pixel_pll), .O(clk_pixel));

    // ---- pixel clock reset: released two clocks after both have locked ----
    wire locked = dcm_locked && pll_locked;
    reg  rst_meta;
    always @(posedge clk_pixel or negedge locked)
        if (!locked) begin
            rst_meta <= 1'b1;
            rst_pixel <= 1'b1;
        end else begin
            rst_meta <= 1'b0;
            rst_pixel <= rst_meta;
        end

    // ---- serialiser ----
    wire [3:0] d0, d1;
    tmds_serialiser ser (.clk_pixel(clk_pixel), .clk_x5(clk_x5), .tmds0(tmds0), .tmds1(tmds1),
                         .tmds2(tmds2), .d0(d0), .d1(d1));
    genvar i;
    generate
        for (i = 0; i < 4; i = i + 1) begin : pair
            wire q;
            ODDR2 #(.DDR_ALIGNMENT("C0"), .INIT(1'b0), .SRTYPE("ASYNC")) ddr (
                .Q(q), .C0(clk_x5), .C1(~clk_x5), .CE(1'b1), .D0(d0[i]), .D1(d1[i]), .R(1'b0), .S(1'b0)
            );
            OBUFDS obuf (.I(q), .O(TMDS_P[i]), .OB(TMDS_N[i]));
        end
    endgenerate
endmodule

// 10 bits per pixel clock to 2 bits per 5x clock, bit 0 first: d0 is the
// first bit of each pair, d1 the second. Lane 3 is the TMDS clock
// (0000011111). The two clocks must come from one PLL, rising together.
module tmds_serialiser (
    input wire       clk_pixel,
    input wire       clk_x5,
    input wire [9:0] tmds0,
    input wire [9:0] tmds1,
    input wire [9:0] tmds2,
    output wire [3:0] d0,
    output wire [3:0] d1
);
    reg [9:0] c0, c1, c2;                       // this pixel's characters, pixel clock
    always @(posedge clk_pixel) begin
        c0 <= tmds0;
        c1 <= tmds1;
        c2 <= tmds2;
    end

    reg [2:0] slot = 3'd0;                      // 0..4: which bit pair goes out
    reg [9:0] s0, s1, s2, sc;
    always @(posedge clk_x5) begin
        slot <= (slot == 3'd4) ? 3'd0 : slot + 1'b1;
        if (slot == 3'd4) begin
            s0 <= c0; s1 <= c1; s2 <= c2; sc <= 10'b0000011111;
        end else begin
            s0 <= s0 >> 2; s1 <= s1 >> 2; s2 <= s2 >> 2; sc <= sc >> 2;
        end
    end

    assign d0 = {sc[0], s2[0], s1[0], s0[0]};
    assign d1 = {sc[1], s2[1], s1[1], s0[1]};
endmodule
