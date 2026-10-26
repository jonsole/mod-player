"""The HDMI text console (issue #21): the soft CPU, the firmware and the
HDMI output together (build/console_tb), every frame decoded and checked
against the console model; the firmware's test screen is also compared
with the reference image tests/golden/console-test-screen.png."""
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modgen  # noqa: E402
import modplayer  # noqa: E402
import png  # noqa: E402

CONSOLE_TB = os.path.join(ROOT, "build", "console_tb")
FONT = os.path.join(ROOT, "hw", "rtl", "console_font.hex")
GOLDEN = os.path.join(ROOT, "tests", "golden", "console-test-screen.png")


def run_console(steps=(), expect=None, frames=2, max_cycles=150000000, song=None):
    tmp = tempfile.mkdtemp()
    ppm = os.path.join(tmp, "last.ppm")
    args = [CONSOLE_TB, "--font", FONT, "--ppm", ppm, "--frames", str(frames), "--max", str(max_cycles)]
    if song:
        path = os.path.join(tmp, "song.mod")
        with open(path, "wb") as f:
            f.write(song)
        args += ["--song", path]
    for i, (after, data) in enumerate(steps):
        path = os.path.join(tmp, "send%d.bin" % i)
        with open(path, "wb") as f:
            f.write(data)
        args += ["--after", after, "--send", path]
    if expect:
        args += ["--expect", expect]
    result = subprocess.run(args, capture_output=True, text=True)
    return result.returncode, result.stdout, ppm


class Console(unittest.TestCase):
    def test_the_test_screen_matches_the_model_and_the_reference_image(self):
        rc, out, ppm = run_console(expect="ready")
        self.assertEqual(rc, 0, out)
        self.assertIn("frames match the model", out)
        got = png.read_ppm(ppm)
        if os.environ.get("UPDATE_GOLDEN"):
            os.makedirs(os.path.dirname(GOLDEN), exist_ok=True)
            png.write_png(GOLDEN, *got)
        # (not assertEqual: its diff of two 900 KB images takes hours)
        self.assertTrue(got == png.read_png(GOLDEN),
                        "the test screen differs from %s (UPDATE_GOLDEN=1 to accept it)" % GOLDEN)

    def test_text_sent_over_serial_shows_on_the_next_frame(self):
        # T writes while frames are being shown: every frame must still be
        # whole characters, old or new (console_tb checks each one), and the
        # new text must be there from the first frame after the writes
        line = bytes(range(64, 117))                                   # 53 characters
        frame = b"".join(modplayer.encode_text(row, 0, 0x1F, line) for row in range(10, 20))
        rc, out, ppm = run_console(steps=[("ready", frame)], expect="OK\r\n" * 10, frames=1)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("(0 cell-frames", out)                        # writes did overlap frames
        _, _, rgb = png.read_ppm(ppm)
        want = render_text(line, 0x1F)
        for row in range(10, 20):
            got = rgb[row * 24 * 640 * 3:(row + 1) * 24 * 640 * 3]
            self.assertTrue(got == want, "row %d of the last frame isn't the text sent" % row)



def status_song():
    """A short Song that moves through Positions, Patterns, speeds and
    tempos quickly: Pattern 0 at speed 3, Pattern 1 at speed 4 and 160 BPM,
    each broken off after 9 rows; Positions 0, 1, 0. All four channels
    play notes, so every scope has a trace."""
    song = modgen.Song("status test")
    song.sample(1, modgen.sine_cycle(32) * 4, volume=50, loop=(0, 128), name="sine wave")
    song.sample(2, modgen.square_cycle(16) * 8, volume=40, loop=(0, 128), name="square")
    song.sample(17, modgen.sine_cycle(16) * 8, volume=30, loop=(0, 128), name="right-hand column")
    song.note(0, 0, 3, effect=0xF, param=3)
    song.note(1, 0, 3, effect=0xF, param=4)
    song.note(1, 0, 2, effect=0xF, param=160)
    for pattern in (0, 1):
        for row in range(0, 9, 2):
            song.note(pattern, row, 0, period=modgen.C2 + 20 * row, sample=1 + pattern)
        song.note(pattern, 8, 1, effect=0xD, param=0)            # on to the next Position
        for row in (1, 3, 5):
            song.note(pattern, row, 1, period=modgen.E2, sample=2)
            song.note(pattern, row + 1, 2, period=modgen.G2, sample=17)
            song.note(pattern, row, 3, period=modgen.C1, sample=1)
    song.positions = [0, 1, 0]
    return song.build()


class StatusScreen(unittest.TestCase):
    """Issue #22: the status screen follows the Song as it plays."""

    def test_the_screen_follows_the_song_row_by_row(self):
        song = status_song()
        rc, out, _ = run_console(steps=[("ready", modplayer.encode_load(song))], expect="OK status test",
                                 frames=110, song=song, max_cycles=200000000)
        self.assertEqual(rc, 0, out)
        self.assertIn("status:", out)
        checked = int(out.split("status: ")[1].split()[0])
        self.assertGreater(checked, 90, out)

    def test_the_scopes_trace_each_voice(self):
        # issue #23: console_tb checks every captured point against the
        # reference Voices and every frame's scope pixels against the traces
        song = status_song()
        rc, out, ppm = run_console(steps=[("ready", modplayer.encode_load(song))], expect="OK status test",
                                   frames=40, song=song, max_cycles=200000000)
        self.assertEqual(rc, 0, out)
        m = re.search(r"scopes: (\d+) frames, (\d+) trace pixels, (\d+) points checked", out)
        self.assertIsNotNone(m, out)
        frames, pixels, points = (int(g) for g in m.groups())
        self.assertGreater(frames, 30, out)
        self.assertGreater(pixels, 1000, out)
        self.assertGreater(points, 4 * 155 * 20, out)

    def test_the_spectrum_is_drawn_under_the_scopes(self):
        # issue #24: the analysis itself is checked against its C model by
        # spectrum_tb; here every frame's bars must be drawn where and as
        # the display's copy of them says
        song = status_song()
        rc, out, ppm = run_console(steps=[("ready", modplayer.encode_load(song))], expect="OK status test",
                                   frames=20, song=song, max_cycles=200000000)
        self.assertEqual(rc, 0, out)
        m = re.search(r"spectrum: (\d+) frames, (\d+) bar pixels", out)
        self.assertIsNotNone(m, out)
        frames, pixels = (int(g) for g in m.groups())
        self.assertGreater(frames, 10, out)
        self.assertGreater(pixels, 10000, out)


PALETTE = [0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
           0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF]


def font():
    with open(FONT) as f:
        return [int(line, 16) for line in f if line.strip() and not line.startswith("//")]


def render_text(chars, attr):
    """One 53-character row of the console (24 lines of 640 pixels, with the
    2-pixel black border each side) as RGB bytes."""
    glyphs = font()
    fg, bg = PALETTE[attr & 15].to_bytes(3, "big"), PALETTE[attr >> 4].to_bytes(3, "big")
    out = bytearray()
    for r in range(24):
        out += bytes(6)
        for ch in chars:
            bits = glyphs[ch * 32 + r]
            for i in range(12):
                out += fg if bits >> (11 - i) & 1 else bg
        out += bytes(6)
    return bytes(out)


if __name__ == "__main__":
    unittest.main()
