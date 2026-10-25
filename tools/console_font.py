"""Makes the text console's font (issue #21, ADR-0012): Spleen 12x24
(hw/rtl/third_party/spleen, BSD 2-clause), 256 glyphs in the IBM PC (code
page 437) layout. Writes a $readmemh file of 8192 12-bit rows: glyph c's
row r (0 = top) is entry c * 32 + r, rows 24..31 unused, bit 11 = leftmost
pixel.

Spleen 12x24 has the letters, digits, punctuation, accents, line drawing
and blocks of code page 437, but not its symbols (card suits, arrows,
Greek, maths). Those come from Spleen's 8x16 code page 437 edition, scaled
by 1.5.

    python3 tools/console_font.py hw/rtl/console_font.hex

The output is committed; `make font-check` checks it is what this makes.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPLEEN = os.path.join(ROOT, "hw", "rtl", "third_party", "spleen")
WIDTH, HEIGHT = 12, 24

# Code page 437's pictures for codes 1-31 and 127 (Python's cp437 codec
# gives control characters there).
LOW = ("☺☻♥♦♣♠•◘○◙♂♀♪♫☼"
       "►◄↕‼¶§▬↨↑↓→←∟↔▲▼")


def cp437(code):
    if code == 0:
        return None                       # blank
    if code < 32:
        return LOW[code - 1]
    if code == 127:
        return "⌂"
    return bytes([code]).decode("cp437")


def read_bdf(path):
    """Returns (cell width, cell height, ascent, {encoding: rows}), each glyph
    placed in the font's cell, rows top first, bit (width - 1) leftmost."""
    glyphs, ascent, cell_w, cell_h = {}, None, None, None
    lines = iter(open(path, encoding="latin-1").read().splitlines())
    for line in lines:
        if line.startswith("FONTBOUNDINGBOX "):
            cell_w, cell_h = (int(v) for v in line.split()[1:3])
        elif line.startswith("FONT_ASCENT "):
            ascent = int(line.split()[1])
        elif line.startswith("STARTCHAR"):
            enc, bbx, rows = None, None, []
            for line in lines:
                if line.startswith("ENCODING "):
                    enc = int(line.split()[1])
                elif line.startswith("BBX "):
                    bbx = [int(v) for v in line.split()[1:]]
                elif line == "BITMAP":
                    for line in lines:
                        if line == "ENDCHAR":
                            break
                        rows.append(int(line, 16))
                    break
            w, h, xoff, yoff = bbx
            nbits = ((w + 7) // 8) * 8
            cell = [0] * cell_h
            top = ascent - (yoff + h)
            for i, bits in enumerate(rows):
                if 0 <= top + i < cell_h:
                    cell[top + i] = ((bits >> (nbits - w)) << (cell_w - w - xoff)) & ((1 << cell_w) - 1)
            if enc is not None and enc >= 0:
                glyphs[enc] = cell
    return cell_w, cell_h, ascent, glyphs


def scale_8x16(rows, ascent):
    """An 8x16 glyph scaled by 1.5 into the 12x24 cell (nearest pixel), its
    baseline moved to the 12x24 font's."""
    out = [0] * HEIGHT
    shift = BIG_ASCENT - ascent * 3 // 2
    for y in range(HEIGHT):
        sy = (y - shift) * 2 // 3
        if 0 <= sy < 16:
            src = rows[sy]
            out[y] = sum(((src >> (7 - x * 2 // 3)) & 1) << (11 - x) for x in range(WIDTH))
    return out


NOTICE = """// The console font: Spleen 2.1.0 in code page 437 order, made by
// tools/console_font.py from hw/rtl/third_party/spleen/: the 12x24 font,
// with the code page's symbols from the 8x16 code page 437 edition scaled
// by 1.5. Glyph c's row r is entry c * 32 + r (rows 24-31 unused); bit 11
// is the leftmost pixel.
//
// Copyright (c) 2018-2024, Frederic Cambus. All rights reserved.
// Spleen is released under the BSD 2-Clause license: see
// hw/rtl/third_party/spleen/LICENSE.
"""

_, _, BIG_ASCENT, BIG = read_bdf(os.path.join(SPLEEN, "spleen-12x24.bdf"))


def main(argv):
    _, _, small_ascent, small = read_bdf(os.path.join(SPLEEN, "spleen-8x16-ibm-437.bdf"))
    out, scaled = [], []
    for code in range(256):
        ch = cp437(code)
        if ch is None:
            cell = [0] * HEIGHT
        elif ord(ch) in BIG:
            cell = BIG[ord(ch)]
        else:
            cell = scale_8x16(small[code], small_ascent)
            scaled.append("%02x" % code)
        out += ["%03x" % v for v in cell] + ["000"] * (32 - HEIGHT)
    with open(argv[1], "w", newline="\n") as f:
        f.write(NOTICE + "\n".join(out) + "\n")
    print("%d glyphs scaled up from 8x16: %s" % (len(scaled), " ".join(scaled)))


if __name__ == "__main__":
    main(sys.argv)
