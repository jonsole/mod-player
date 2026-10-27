"""Makes the text console's font (issue #21, ADR-0012, ADR-0016): 256
glyphs in the IBM PC (code page 437) layout. Printable ASCII (32-126) and
the horizontal line (0xC4) are 16-segment characters, like a vacuum
fluorescent display's (tools/segment_font.py); 0x7F is every segment, the
pattern the console shows faintly where a character is unlit. The rest is
Spleen 12x24 (hw/rtl/third_party/spleen, BSD 2-clause). Writes a $readmemh file of 8192 12-bit rows: glyph c's
row r (0 = top) is entry c * 32 + r, rows 24..31 unused, bit 11 = leftmost
pixel.

Spleen 12x24 has the letters, digits, punctuation, accents, line drawing
and blocks of code page 437, but not its symbols (card suits, arrows,
Greek, maths). Those come from Spleen's 8x16 code page 437 edition, scaled
by 1.5.

    python3 tools/console_font.py hw/rtl/console_font.hex

It also writes console_segments.hex beside it, 512 rows of 12 bits: five
blocks of 32 rows (24 used): the unlit-segment pattern (as glyph 0x7F),
then the half-brightness edge pixels of the diagonals H, J, K and M; then
from row 256, for each character, the diagonals it lights (H 8, J 4, K 2,
M 1): the console adds those diagonals' edges, anti-aliasing them.

The output is committed; `make font-check` checks it is what this makes.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import segment_font  # noqa: E402

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


NOTICE = """// The console font, in code page 437 order, made by tools/console_font.py:
// 16-segment characters (tools/segment_font.py) for printable ASCII and
// 0xC4, every segment at 0x7F, and the rest Spleen 2.1.0 from
// hw/rtl/third_party/spleen/: the 12x24 font, with the code page's symbols
// from the 8x16 code page 437 edition scaled by 1.5. Glyph c's row r is
// entry c * 32 + r (rows 24-31 unused); bit 11 is the leftmost pixel.
//
// Copyright (c) 2018-2024, Frederic Cambus. All rights reserved.
// Spleen is released under the BSD 2-Clause license: see
// hw/rtl/third_party/spleen/LICENSE.
"""

SEGMENTS_NOTICE = """// The console's 16-segment patterns, made by tools/console_font.py from
// tools/segment_font.py (ADR-0016): rows 0-31 every segment (shown faintly
// where a character is unlit), then 32 rows each of the half-brightness edge
// pixels of the diagonals H, J, K and M. Rows 24-31 of each are unused; bit
// 11 is the leftmost pixel. Rows 160-255 are unused. Rows 256-511: for
// each character, the diagonals it lights (H 8, J 4, K 2, M 1).
"""

_, _, BIG_ASCENT, BIG = read_bdf(os.path.join(SPLEEN, "spleen-12x24.bdf"))


def main(argv):
    _, _, small_ascent, small = read_bdf(os.path.join(SPLEEN, "spleen-8x16-ibm-437.bdf"))
    out, scaled = [], []
    for code in range(256):
        ch = cp437(code)
        if code == 0x7F:
            cell = segment_font.rows(segment_font.GHOST)
        elif 32 <= code < 127 or code == 0xC4:
            ch = chr(code)                    # ASCII, or "Ä" for the rule (segment_font's key)
            if not segment_font.has(ch):
                sys.exit("no 16-segment character for code %02x" % code)
            cell = segment_font.rows(segment_font.glyph(ch))
        elif ch is None:
            cell = [0] * HEIGHT
        elif ord(ch) in BIG:
            cell = BIG[ord(ch)]
        else:
            cell = scale_8x16(small[code], small_ascent)
            scaled.append("%02x" % code)
        out += ["%03x" % v for v in cell] + ["000"] * (32 - len(cell))
    with open(argv[1], "w", newline="\n") as f:
        f.write(NOTICE + "\n".join(out) + "\n")
    segs = []
    for pixels in [segment_font.GHOST] + [segment_font.EDGE[d] for d in segment_font.DIAGONALS]:
        segs += ["%03x" % v for v in segment_font.rows(pixels)] + ["000"] * (32 - HEIGHT)
    segs += ["000"] * (256 - len(segs))
    for code in range(256):                   # each character's diagonals
        ch = chr(code)
        segs.append("%03x" % (segment_font.diagonals(ch) if 32 <= code < 127 or code == 0xC4 else 0))
    with open(os.path.join(os.path.dirname(os.path.abspath(argv[1])), "console_segments.hex"), "w", newline="\n") as f:
        f.write(SEGMENTS_NOTICE + "\n".join(segs) + "\n")
    print("%d glyphs scaled up from 8x16: %s" % (len(scaled), " ".join(scaled)))


if __name__ == "__main__":
    main(sys.argv)
