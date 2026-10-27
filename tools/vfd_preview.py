#!/usr/bin/env python3
"""A Python model of a white vacuum-fluorescent-display look for the HDMI
status screen, to try it out before building it in hardware:

  - text in 16-segment characters, one per 12 x 24 cell, with every
    character position's unlit segments faintly visible, as on a VFD module
  - the Voice scopes in white, the two older captures fading
  - the spectrum as stacks of short, wide LEDs, unlit ones faintly visible,
    the peak a single lit LED

Everything is drawn with the few brightness levels the hardware can give
(no glow), on the same layout as firmware/status.c.

    build/voice_dump song.mod 20.0 0.6 voices.bin      (see tools/scope_preview.py)
    python3 tools/vfd_preview.py voices.bin screen.png ["Song name"]
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import png                # noqa: E402
import scope_preview as sp  # noqa: E402

W, H = 640, 480
CW, CH = 12, 24
LEFT = 2                                   # the console's first column (53 x 12 = 636)
LIT = (0xF4, 0xF4, 0xF4)
DIM = (0x9A, 0x9A, 0x9A)                   # labels: a lower grid voltage
GHOST = (0x12, 0x12, 0x12)
BAR = (0xB0, 0xB0, 0xB0)                   # the spectrum's lit LEDs; the peak is LIT                 # unlit segments and LEDs
FADE = [(0xF4,) * 3, (0xB7,) * 3, (0x7A,) * 3]   # scopes: newest, 3/4, 1/2
CENTRE = (0x30, 0x30, 0x30)

# ---- 16-segment characters: tools/segment_font.py ---------------------------------
from segment_font import glyph, edges, GHOST as GHOST_PIX  # noqa: E402


# ---- the screen -------------------------------------------------------------------
class Screen:
    def __init__(self):
        self.img = bytearray(W * H * 3)

    def put(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            self.img[o:o + 3] = bytes(c)

    def text(self, row, col, s, colour=LIT):
        for i, ch in enumerate(s):
            for (x, y) in glyph(ch):
                self.put(LEFT + (col + i) * CW + x, row * CH + y, colour)
            for (x, y) in edges(ch):                      # anti-aliased diagonals: half brightness
                self.put(LEFT + (col + i) * CW + x, row * CH + y, tuple(c // 2 for c in colour))

    def ghosts(self, rows):
        for row in rows:
            for col in range(53):
                for (x, y) in GHOST_PIX:
                    self.put(LEFT + col * CW + x, row * CH + y, GHOST)


def status_text(scr, song):
    scr.ghosts(range(4))
    scr.text(0, 1, "MOD-PLAYER 0.1", LIT)
    scr.text(0, 45, "PLAYING", LIT)
    scr.text(1, 1, song, LIT)
    c = 1
    for label, value in (("POS ", "003/029"), ("  PAT ", "007"), ("  ROW ", "12"), ("  SPD ", "06"), ("  BPM ", "125")):
        scr.text(2, c, label, DIM)
        c += len(label)
        scr.text(2, c, value, LIT)
        c += len(value)
    scr.text(3, 0, "\xc4" * 53, DIM)


def scopes(scr, newest_first):
    for v in range(4):
        x0, y0 = 2 + 320 * (v & 1), 96 + 96 * (v >> 1)
        for x in range(sp.POINTS):
            scr.put(x0 + x, y0 + 47, CENTRE)
        for age in (2, 1, 0):
            if age >= len(newest_first):
                continue
            pts = newest_first[age][v]
            for x in range(sp.POINTS):
                p0, p1, p2 = pts[max(x - 1, 0)], pts[x], pts[min(x + 1, sp.POINTS - 1)]
                ends = (p0 + p1, 2 * p1, p1 + p2)
                lo2, hi2 = min(ends), max(ends)
                for dy in range(lo2 // 2 - 1, hi2 // 2 + 2):
                    if lo2 <= 2 * dy <= hi2 and -48 <= dy <= 47:
                        scr.put(x0 + x, y0 + 47 - dy, FADE[age])
        scr.text(4 + 4 * (v >> 1), 0 if v % 2 == 0 else 27, str(v + 1), DIM)


# spectrum: 48 bars 13 apart from x = 8, standing on y = 479, up to 180 high
BARS = 24                                  # bands across 624 pixels from x = 8
LED_GAP_X = 3
LED_H, LED_PITCH = 5, 7
LEDS = 25                                  # 25 x 7 = 175


def set_led(height, gap):
    global LED_H, LED_PITCH, LEDS
    LED_H, LED_PITCH = height, height + gap
    LEDS = 180 // LED_PITCH


def spectrum_bars(d, at, n=1024):
    """Bar heights (0..180), much as hw/rtl/spectrum.v makes them: a Hann-windowed
    1024-point FFT of the mix at 48 kHz, each bar the loudest of its bins, in
    eighths of an octave of power above a floor."""
    import numpy as np
    step = sp.CCK_HZ / 48000
    idx = (at - (np.arange(n)[::-1] * step)).astype(int).clip(0, d.n - 1)
    v = np.frombuffer(bytes(d.v), dtype=np.int16).reshape(-1, 4).astype(float)
    x = v[idx].sum(axis=1) / 2                    # the mix, roughly at the Resampler's scale
    w = 0.5 * (1 - np.cos(2 * np.pi * np.arange(n) / n))
    f = np.fft.fft(x * w) / n
    p = np.abs(f) ** 2
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import spectrum_tables as st
    out = []
    st.BARS = BARS
    for lo, hi in st.bars():
        m = p[lo:hi + 1].max()
        level = 8 * math.log2(m) if m > 0 else 0
        out.append(int(max(0, min(180, level - 16))))
    return out


def spectrum(scr, heights, peaks):
    pitch = 624 // BARS
    led_w = pitch - LED_GAP_X
    for b in range(BARS):
        x0 = 8 + (624 - pitch * BARS) // 2 + pitch * b
        lit = heights[b] // LED_PITCH
        peak = peaks[b] // LED_PITCH
        for k in range(LEDS):
            y_bottom = 479 - k * LED_PITCH
            colour = LIT if k == peak and peak > 0 else BAR if k < lit else GHOST
            for y in range(y_bottom - LED_H + 1, y_bottom + 1):
                for x in range(x0, x0 + led_w):
                    scr.put(x, y, colour)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    global BARS
    for a in argv[3:]:
        if a.startswith("--bars="):
            BARS = int(a[7:])
        if a.startswith("--led="):                 # height,gap
            h, g = a[6:].split(",")
            set_led(int(h), int(g))
    argv = [a for a in argv if not a.startswith("--")]
    song = argv[3] if len(argv) > 3 else "XENON 2 MEGABLAST"
    d = sp.Dump(argv[1])
    caps = sp.captures(d, "line", 256)
    pick = min(len(caps) - 1, 5)
    newest_first = [caps[pick - i][1] for i in range(3) if pick - i >= 0]
    at = caps[pick][0]
    # bars now; peaks as the highest bar over the last 0.3 s (a stand-in for the hold and fall)
    heights = spectrum_bars(d, at)
    peaks = heights[:]
    for back in range(1, 18):
        t = at - back * sp.FRAME
        if t > 2048 * 74:
            peaks = [max(p, h - back * 4) for p, h in zip(peaks, spectrum_bars(d, t))]
    scr = Screen()
    status_text(scr, song)
    scopes(scr, newest_first)
    spectrum(scr, heights, peaks)
    png.write_png(argv[2], W, H, bytes(scr.img))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
