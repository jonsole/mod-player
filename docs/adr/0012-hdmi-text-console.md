# The HDMI picture is a 53 x 20 text console in Spleen 12 x 24

The status screen (issues #21-#23) is text: a console of 53 x 20 character cells of 12 x 24 pixels on the 640 x 480 picture, centred with a 2-pixel black border each side. Each cell holds a character in code page 437 order (so line drawing and block characters are there for scopes and bars) and an attribute byte: foreground and background from the 16 VGA colours. The font is Spleen 12 x 24 (BSD 2-clause, vendored in `hw/rtl/third_party/spleen`); Spleen 12 x 24 lacks code page 437's symbols (card suits, arrows, Greek), which come from Spleen's 8 x 16 code page 437 edition scaled by 1.5. `tools/console_font.py` makes `hw/rtl/console_font.hex`, which is committed.

The owner chose the size and typeface over the first version, the VGA 8 x 16 font at 80 x 30: larger, more modern letters, with 20 rows still enough for the Song information and all 31 Sample names in two columns.

The CPU writes a cell as one 32-bit store at `0x2001_0000 + 4 * cell` (character in bits 7:0, attribute in 15:8). The text RAM is written and read only on the pixel clock: each CPU write is held in a register and handed across with a toggle handshake, and the next write waits for its acknowledgement (about 150 ns). A frame counter and a vertical-blanking flag (`0x2001_4000`) let firmware put whole updates in the blanking, so a screen is never seen half redrawn.

## Considered options

- **True dual-port block RAM, CPU on one clock and video on the other**: simpler, but a read of a cell in the same cycle as its write on the other clock returns undefined data on Spartan-6, a flicker of garbage the simulation can't show.
- **A bitmap frame buffer**: 640 x 480 even at 1 bit per pixel is 300 kbit, more than the block RAM left, and drawing text in software is slow.
- **A higher resolution (720p) for more text at a large size**: a 74.25 MHz pixel clock and 742.5 Mbit/s serialisation, beyond the ODDR2-based serialiser (ADR-0010).
- **VGA 8 x 16 at 80 x 30**: more text, but small on a TV across a room; replaced at the owner's request.

## Consequences

- The picture is 5 pixel clocks behind `hdmi_tx`'s timing (two block RAM reads after a stage to find the cell); `hdmi_tx` delays the rest of its output to match (`RGB_DELAY`).
- Block RAM: 6 RAMB16s for the font (8192 x 12 bits; rows 24-31 of each glyph unused, so the address is just character x 32 + row) and up to 4 for the text RAM.
- `make console-test` decodes every frame of the HDMI output with the CPU and firmware running and checks it pixel for pixel against a model of the console; the firmware's test screen is also compared with `tests/golden/console-test-screen.png`.
- The non-HDMI build has no console: the status register reads 0 there, and the firmware's console calls do nothing.
