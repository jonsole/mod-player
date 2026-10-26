#!/usr/bin/env python3
"""A Python model of the HDMI Voice scopes (hw/rtl/scopes.v, ADR-0013), to
try out ways of drawing them before building them in hardware.

    cc -O2 -Ihost -Ifirmware/replayer -o build/voice_dump tools/voice_dump.c \\
       host/paula_model.c firmware/replayer/mod.c firmware/replayer/periods.c firmware/replayer/replayer.c
    build/voice_dump song.mod 3.0 0.6 voices.bin
    python3 tools/scope_preview.py voices.bin sheet.png

Writes one picture of the scopes' rows (640 x 192 each, the 2 x 2 grid as the
board draws it) for each way of capturing:

  hold-64     a point every 64 colour clocks (5.7 ms across); a Voice's
              output is held between Paula's sample changes, so this draws
              steps
  hold-256    the same, every 256 clocks (22.8 ms across)
  line-256    every 256 clocks, but the value between two sample changes is
              the straight line joining them, so there are no steps
  sample      one point per new sample of the Voice, joined up (a Voice's
              own sample data, however long that takes: 316 samples)

Each follows the hardware: once a frame (59 115 colour clocks) each Voice
waits for a rising zero crossing (or 36 000 clocks), then takes 316 points,
each (output >> 7) * 0.75; the next capture starts at the first frame
boundary after all four are done; a frame shows the newest capture at full
brightness and the two before it at half and a quarter.
"""
import array
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import png  # noqa: E402

CCK_HZ = 3546895
FRAME = 59115
POINTS = 316
TIMEOUT = 36000
SILENT_STRIDE = 64                      # a Voice that never changes
COLOURS = [(0x55, 0xFF, 0x55), (0x55, 0xFF, 0xFF), (0xFF, 0xFF, 0x55), (0xFF, 0x55, 0xFF)]
MODES = [("hold", 64), ("hold", 256), ("line", 256), ("sample", 0)]


def point(v):
    p = int(v) >> 7                     # 8-bit signed (-64..63)
    return p - (p >> 2)                 # times 0.75: -48..48


class Dump:
    def __init__(self, path):
        self.v = array.array("h")
        with open(path, "rb") as f:
            self.v.frombytes(f.read())
        self.n = len(self.v) // 4
        # when each Voice's output changes: Paula's sample changes
        self.changes = [[i for i in range(1, self.n) if self.v[4 * i + k] != self.v[4 * (i - 1) + k]]
                        for k in range(4)]

    def at(self, k, i):
        return self.v[4 * min(i, self.n - 1) + k]

    def line(self, k, t):
        """The Voice's output at time t with straight lines between its sample changes."""
        c = self.changes[k]
        j = bisect.bisect_right(c, t) - 1
        if j < 0 or j + 1 >= len(c):
            return self.at(k, t)
        t0, t1 = c[j], c[j + 1]
        v0, v1 = self.at(k, t0), self.at(k, t1)
        return v0 + (v1 - v0) * (t - t0) / (t1 - t0)

    def trigger(self, k, start):
        i, prev = start, self.at(k, start - 1)
        while i < self.n and i - start < TIMEOUT:
            cur = self.at(k, i)
            if prev < 0 <= cur:
                break
            prev, i = cur, i + 1
        return i


def capture(d, start, mode, stride):
    """One capture from frame boundary `start`: (points per Voice, done at)."""
    caps, done = [], start
    for k in range(4):
        i = d.trigger(k, start)
        if mode == "hold":
            pts = [point(d.at(k, i + j * stride)) for j in range(POINTS)]
            end = i + (POINTS - 1) * stride
        elif mode == "line":
            pts = [point(round(d.line(k, i + j * stride))) for j in range(POINTS)]
            end = i + (POINTS - 1) * stride
        else:                                       # one point per sample change
            c = d.changes[k]
            pos = bisect.bisect_left(c, i)
            times = c[pos:pos + POINTS]
            pts = [point(d.at(k, t)) for t in times]
            if len(times) < POINTS:                 # it stopped changing: held, at the silent rate
                pts += [pts[-1] if pts else point(d.at(k, i))] * (POINTS - len(pts))
                end = (times[-1] if times else i) + (POINTS - len(times)) * SILENT_STRIDE
            else:
                end = times[-1]
        caps.append(pts)
        done = max(done, end)
    return caps, done


def captures(d, mode, stride):
    """All captures over the dump: [(done_at, points)], as the hardware makes them."""
    out, start = [], FRAME
    while start + TIMEOUT + POINTS * max(stride, 1100) < d.n:
        caps, done = capture(d, start, mode, stride)
        out.append((done, caps))
        start = (done // FRAME + 1) * FRAME        # the next frame boundary after it
    return out


def draw(newest_first):
    """The scopes' rows: 640 x 192 RGB, given up to three captures, newest first."""
    img = bytearray(640 * 192 * 3)

    def put(x, y, c):
        o = (y * 640 + x) * 3
        img[o:o + 3] = bytes(c)

    for v in range(4):
        x0, y0 = 2 + 320 * (v & 1), 96 * (v >> 1)
        for x in range(POINTS):                                        # the centre line
            put(x0 + x, y0 + 48, (0x55, 0x55, 0x55))
        for age in (2, 1, 0):                                          # oldest first
            if age >= len(newest_first):
                continue
            pts = newest_first[age][v]
            colour = tuple(c // (1, 2, 4)[age] for c in COLOURS[v])
            for x in range(POINTS):
                here, before = pts[x], pts[x - 1] if x else pts[x]
                for dy in range(min(here, before), max(here, before) + 1):
                    y = y0 + 48 - dy
                    if y0 <= y < y0 + 96:
                        put(x0 + x, y, colour)
    return bytes(img)


def main(argv):
    d = Dump(argv[1])
    rows = []
    for mode, stride in MODES:
        caps = captures(d, mode, stride)
        name = "%s-%d" % (mode, stride) if stride else mode
        every = d.n / len(caps) / CCK_HZ * 1000
        span = POINTS * stride / CCK_HZ * 1000 if stride else float("nan")
        print("%-9s %d captures in %.2f s (one every %.0f ms); %s" % (
            name, len(caps), d.n / CCK_HZ, every,
            "%.1f ms across" % span if stride else "as long as 316 samples take"))
        rows.append(draw([c for _, c in caps[-3:]][::-1]))
        rows.append(bytes(640 * 6 * 3))                               # a gap
    png.write_png(argv[2], 640, 198 * len(MODES), b"".join(rows))
    print("wrote %s: rows top to bottom: %s" % (argv[2], ", ".join(
        "%s-%d" % m if m[1] else m[0] for m in MODES)))


if __name__ == "__main__":
    main(sys.argv)
