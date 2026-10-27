# The HDMI picture looks like a white vacuum fluorescent display

At the owner's request the whole picture has a retro, monochrome look: a white vacuum fluorescent display (VFD), with segment-display text and an LED-style spectrum. It was modelled first in Python (`tools/vfd_preview.py`, which draws the status screen from a Song's real Voices) and the owner chose its details from the pictures: white rather than a VFD's usual blue-green, everything monochrome, unlit segments and LEDs faintly visible, 24 spectrum bands, LEDs 5 pixels tall.

- **Text** is in 16-segment characters (`tools/segment_font.py`), one per 12 x 24 cell: upper case only, as on real segment displays. The segments are 1 pixel thick: with 2-pixel strokes the characters looked like a lower-resolution font scaled up. They replace Spleen for printable ASCII and the horizontal line (0xC4); the rest of code page 437 stays Spleen, since no segment display has those symbols. Colours become brightnesses: 9-15 white, 1-8 dim, 0 off. A cell with any background shows every segment faintly where its character is unlit, as a VFD module shows its unlit segments. The diagonal segments are anti-aliased, at the owner's request: pixels a diagonal covers at least 60% are lit, those it covers 25-60% are at half brightness. The unlit pattern, the four diagonals' edge pixels and which diagonals each character lights are in `console_segments.hex`, made with the font, as small ROMs in logic (about 180 LUTs), so none of this costs block RAM. (Keeping the diagonals in a spare row of each glyph and reading it on the font memory's second port crashed XST.) The status screen gives its top four rows (title, Song, position, rule) a background, and the rows under the scopes and spectrum none.
- **Scopes** are white, with a faint centre line. Persistence is off for now: the owner found the newest capture with the two before it (at three quarters and half brightness) looked like three waveforms overlaid, not a phosphor. Only the newest is shown (`GENERATIONS` in `hw/rtl/scopes.v`, and the same in `console_tb`); a real phosphor, a per-pixel brightness that fades every frame, would need its 60 KB in SDRAM and is left for later.
- **The spectrum** has 24 bars, each a stack of 25 LEDs of 23 x 5 pixels (26 apart across, 7 up). LED k is lit when the bar is at least 7 (k + 1) high; the peak is the single LED it falls in (from the second up), brighter than the bar; unlit LEDs glow faintly. The analysis (ADR-0014) is unchanged except for the number of bars, so each covers twice the band, and the range: the owner found 68 dB (a pixel a level) lit every bar with quiet background energy, so the bars now span the top 36 dB (floor at level 100, 15/8 pixel a level), chosen from animated mockups of 68, 48, 36 and 30 dB.

The brightnesses are white F4 (also the spectrum's peak LED), the spectrum's bar LEDs B0, dim 9A, faded scope traces B7 and 7A, unlit segments and LEDs 12 (first 1E; the owner wanted them dimmer), the scopes' centre line 30.

## Considered options

- **A blue-green VFD**: the usual colour of real VFDs; the owner preferred white.
- **Italic (slanted) segments**, as many VFDs have: at 12 x 24 pixels the slant makes the segments jagged.
- **A glow around lit segments**: needs blending the font with its neighbours, more than the console's one-pixel-at-a-time pipeline does; left out.
- **Unlit segments everywhere**: under the scopes and the spectrum they would show through the traces, so only cells with a background have them.

## Consequences

- `console_tb` and `tests/test_console.py` model the brightnesses and the unlit segments; the test screen's reference image shows every glyph. `spectrum_tb` and `console_tb` share the LED drawing model (`hw/sim/spectrum_leds.h`).
- Firmware that wants coloured text gets brightness instead; the boot loader and test screens are plain white and dim.
