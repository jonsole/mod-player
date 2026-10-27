"""16-segment characters for the console font (ADR-0016): the look of a
vacuum fluorescent display's alphanumeric digits, one per 12 x 24 cell.

The segments join at pixel columns 1 (left), 5 (middle) and 9 (right) and
rows 2 (top), 11 (middle) and 20 (bottom): two halves of the top, middle
and bottom bars, the left and right sides above and below the middle, a
centre stroke above and below, and four diagonals. Segments are 1 pixel
thick, like a VFD's thin ones (2-pixel strokes looked like a coarser font
scaled up), and each is trimmed at its ends so the segments stand apart. A dot (bottom right) makes '.' and ','; two dots make ':'.
Letters are upper case only, as on real segment displays.

The four diagonals are anti-aliased: from each pixel's coverage by the
diagonal (sampled 8 x 8), the pixels covered at least AA_FULL are its core,
lit like any segment, and those covered at least AA_EDGE are its edge,
drawn at half brightness (EDGE). A character's lit diagonals are
diagonals(ch), so the console can add the edges of just those.

glyph(ch) gives the lit pixels of a character; GHOST gives every segment's
pixels, the pattern a VFD shows faintly where a character is unlit.
"""
import math

CW, CH = 12, 24
X0, XM, X1 = 1.5, 5.5, 9.5                 # pixel centres
Y0, YM, Y1 = 2.5, 11.5, 20.5
SEGS = {
    "A1": ((X0, Y0), (XM, Y0)), "A2": ((XM, Y0), (X1, Y0)),
    "G1": ((X0, YM), (XM, YM)), "G2": ((XM, YM), (X1, YM)),
    "D1": ((X0, Y1), (XM, Y1)), "D2": ((XM, Y1), (X1, Y1)),
    "F": ((X0, Y0), (X0, YM)), "E": ((X0, YM), (X0, Y1)),
    "B": ((X1, Y0), (X1, YM)), "C": ((X1, YM), (X1, Y1)),
    "I": ((XM, Y0), (XM, YM)), "L": ((XM, YM), (XM, Y1)),
    "H": ((X0, Y0), (XM, YM)), "J": ((X1, Y0), (XM, YM)),
    "K": ((XM, YM), (X0, Y1)), "M": ((XM, YM), (X1, Y1)),
}
GAP = 1.0                                  # trimmed from each end of a segment
HALF = 0.55                                # half the stroke (1 pixel)
HALF_DIAG = 0.5                            # half a diagonal's stroke, for its coverage
AA_FULL, AA_EDGE = 0.6, 0.25               # coverage for a diagonal's core, its edge
DIAGONALS = ("H", "J", "K", "M")           # in the order of their bits, 3..0

CHARS = {
    " ": "", "0": "A1 A2 B C D1 D2 E F J K", "1": "B C J", "2": "A1 A2 B G1 G2 E D1 D2",
    "3": "A1 A2 B C D1 D2 G2", "4": "F G1 G2 B C", "5": "A1 A2 F G1 G2 C D1 D2",
    "6": "A1 A2 F E D1 D2 C G1 G2", "7": "A1 A2 B C", "8": "A1 A2 B C D1 D2 E F G1 G2",
    "9": "A1 A2 B C D1 D2 F G1 G2",
    "A": "A1 A2 B C E F G1 G2", "B": "A1 A2 B C D1 D2 I L G2", "C": "A1 A2 F E D1 D2",
    "D": "A1 A2 B C D1 D2 I L", "E": "A1 A2 F E D1 D2 G1", "F": "A1 A2 F E G1",
    "G": "A1 A2 F E D1 D2 C G2", "H": "F E B C G1 G2", "I": "A1 A2 D1 D2 I L",
    "J": "B C D1 D2 E", "K": "F E G1 J M", "L": "F E D1 D2", "M": "F E B C H J",
    "N": "F E B C H M", "O": "A1 A2 B C D1 D2 E F", "P": "A1 A2 B F E G1 G2",
    "Q": "A1 A2 B C D1 D2 E F M", "R": "A1 A2 B F E G1 G2 M", "S": "A1 A2 F G1 G2 C D1 D2",
    "T": "A1 A2 I L", "U": "F E B C D1 D2", "V": "F E K J", "W": "F E B C K M",
    "X": "H J K M", "Y": "H J L", "Z": "A1 A2 J K D1 D2",
    "-": "G1 G2", "/": "J K", "\\": "H M", "+": "G1 G2 I L", "*": "G1 G2 H I J K L M",
    "=": "G1 G2 D1 D2", "_": "D1 D2", "(": "J M", ")": "H K", "<": "J M", ">": "H K",
    "[": "A1 F E D1", "]": "A2 B C D2", "{": "A2 I G1 L D2", "}": "A1 I G2 L D1",
    "|": "I L", "^": "K M", "~": "G1 G2", "`": "H",
    "'": "I", '"': "F I", "$": "A1 A2 F G1 G2 C D1 D2 I L",
    "%": "A1 F G1 I J K L G2 C D2", "#": "B C I L G1 G2 D1 D2", "?": "A1 A2 B G2 L",
    "!": "I", "&": "A1 H J G1 E D1 D2 M", "@": "A1 A2 B G2 I E D1 D2 F",
    ".": "", ",": "", ":": "", ";": "",
    "\xc4": "G1 G2",                         # code page 437's horizontal line: a rule
}
DOT = [(10, 19), (11, 19), (10, 20), (11, 20)]           # the decimal point, bottom right
LOW_DOT = [(5, 19), (5, 20)]                              # on the centre column, at the bottom
COLON = [(5, 7), (5, 8), (5, 15), (5, 16)]
DOTS = {".": DOT, ",": DOT, "!": LOW_DOT, "?": LOW_DOT, ":": COLON, ";": COLON}


def inside(name, px, py, half):
    (ax, ay), (bx, by) = SEGS[name]
    dx, dy = bx - ax, by - ay
    n = math.hypot(dx, dy)
    ux, uy = dx / n, dy / n
    t = (px - ax) * ux + (py - ay) * uy
    d = abs((px - ax) * uy - (py - ay) * ux)
    return GAP <= t <= n - GAP and d < half


def segment_pixels(name):
    """A straight segment's pixels: those whose centres it covers."""
    return {(x, y) for y in range(CH) for x in range(CW) if inside(name, x + 0.5, y + 0.5, HALF)}


def coverage(name, n=8):
    """How much of each pixel a diagonal covers (0..1), sampled n x n."""
    cov = {}
    for y in range(CH):
        for x in range(CW):
            hits = sum(inside(name, x + (i + 0.5) / n, y + (j + 0.5) / n, HALF_DIAG)
                       for i in range(n) for j in range(n))
            if hits:
                cov[(x, y)] = hits / (n * n)
    return cov


SEG_PIX, EDGE = {}, {}
for _name in SEGS:
    if _name in DIAGONALS:
        _cov = coverage(_name)
        SEG_PIX[_name] = {p for p, c in _cov.items() if c >= AA_FULL}
        EDGE[_name] = {p for p, c in _cov.items() if AA_EDGE <= c < AA_FULL}
    else:
        SEG_PIX[_name] = segment_pixels(_name)
GHOST = set().union(*SEG_PIX.values()) | set(DOT) | set(LOW_DOT) | set(COLON)


def has(ch):
    c = ch.upper() if ch.isalpha() else ch
    return c in CHARS


def glyph(ch):
    """The lit pixels (x, y) of character ch in its cell."""
    c = ch.upper() if ch.isalpha() else ch
    pix = set()
    for s in CHARS.get(c, CHARS["?"]).split():
        pix |= SEG_PIX[s]
    pix |= set(DOTS.get(c, []))
    return pix


def diagonals(ch):
    """The diagonals character ch lights, as bits: H 8, J 4, K 2, M 1."""
    c = ch.upper() if ch.isalpha() else ch
    segs = CHARS.get(c, CHARS["?"]).split()
    return sum(8 >> i for i, d in enumerate(DIAGONALS) if d in segs)


def edges(ch):
    """The half-brightness pixels of character ch: its diagonals' edges, less its lit pixels."""
    pix = set()
    for i, d in enumerate(DIAGONALS):
        if diagonals(ch) & (8 >> i):
            pix |= EDGE[d]
    return pix - glyph(ch)


def rows(pixels):
    """A glyph as 24 rows of 12 bits, bit 11 the leftmost pixel."""
    out = [0] * CH
    for (x, y) in pixels:
        out[y] |= 1 << (CW - 1 - x)
    return out
