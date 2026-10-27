"""The console font's 16-segment characters (ADR-0016): checked in the font
file the hardware is built with, since the testbenches' models read the same
file and so cannot see a wrong glyph."""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import segment_font  # noqa: E402

FONT = os.path.join(ROOT, "hw", "rtl", "console_font.hex")
SEGMENTS = os.path.join(ROOT, "hw", "rtl", "console_segments.hex")


def hex_rows(path):
    with open(path) as f:
        return [int(line, 16) for line in f if line.strip() and not line.startswith("//")]


def glyphs():
    rows = hex_rows(FONT)
    return [tuple(rows[c * 32:c * 32 + 24]) for c in range(256)]


def diagonal_rows():
    return hex_rows(SEGMENTS)[256:512]


class SegmentFont(unittest.TestCase):
    def test_every_segment_character_is_its_own_not_the_question_mark(self):
        g = glyphs()
        for code in list(range(33, 127)) + [0xC4]:
            if code in (ord("?"), ord("!")):              # '!' is '?' without its top: still differs
                continue
            self.assertNotEqual(g[code], g[ord("?")], "code %02x draws as '?'" % code)

    def test_the_rule_is_the_middle_bar(self):
        want = segment_font.rows(segment_font.SEG_PIX["G1"] | segment_font.SEG_PIX["G2"])
        self.assertEqual(glyphs()[0xC4], tuple(want))

    def test_lower_case_is_upper_case(self):
        g = glyphs()
        for c in "abcdefghijklmnopqrstuvwxyz":
            self.assertEqual(g[ord(c)], g[ord(c.upper())], c)

    def test_0x7f_is_every_segment_and_covers_every_character(self):
        g = glyphs()
        ghost = g[0x7F]
        self.assertEqual(ghost, tuple(segment_font.rows(segment_font.GHOST)))
        for code in list(range(32, 127)) + [0xC4]:
            for r in range(24):
                self.assertEqual(g[code][r] & ~ghost[r], 0, "code %02x lights outside the segments" % code)


    def test_the_segments_file_says_which_diagonals_and_only_segment_glyphs_have_them(self):
        diag = diagonal_rows()
        self.assertEqual(diag[ord("N")], 0b1001)                 # H and M
        self.assertEqual(diag[ord("X")], 0b1111)
        self.assertEqual(diag[ord("A")], 0)
        for code in range(256):
            if not (32 <= code < 127 or code == 0xC4):
                self.assertEqual(diag[code], 0, "code %02x" % code)

    def test_the_segments_file_holds_the_unlit_pattern_and_each_diagonals_edge(self):
        segs = hex_rows(SEGMENTS)
        self.assertEqual(len(segs), 512)
        self.assertEqual(tuple(segs[0:24]), tuple(segment_font.rows(segment_font.GHOST)))
        for i, d in enumerate(segment_font.DIAGONALS):
            edge = segs[32 * (i + 1):32 * (i + 1) + 24]
            self.assertEqual(edge, segment_font.rows(segment_font.EDGE[d]), d)
            self.assertTrue(any(edge), d)
            core = segment_font.rows(segment_font.SEG_PIX[d])
            self.assertFalse(any(e & c for e, c in zip(edge, core)), "%s's edge overlaps its core" % d)


if __name__ == "__main__":
    unittest.main()
