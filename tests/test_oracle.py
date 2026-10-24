"""The Replayer must make exactly the same Paula writes as pt2-clone (ADR-0007).

Both sides print a register-write log: one header line per tick, then that
tick's writes, e.g.

    tick 1 pos 0 row 0 bpm 125
      AUD0PER 428
      AUD0PTR +0        (offset into the Song's sample data, or "null")
      DMACON 8001
"""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modgen  # noqa: E402
from compare_logs import first_difference  # noqa: E402

BUILD = os.path.join(ROOT, "build")
SONGS = os.path.join(ROOT, "tests", "songs")
TICKS = "3000"        # about a minute at the default tempo
REAL_TICKS = "20000"  # several minutes: real Songs play through at least once


def logs_for(mod_bytes, ticks=TICKS):
    tmp = tempfile.mkdtemp()
    path = os.path.join(tmp, "song.mod")
    with open(path, "wb") as f:
        f.write(mod_bytes)
    out = []
    for tool in ("pt2_log", "replayer_log"):
        result = subprocess.run([os.path.join(BUILD, tool), path, ticks],
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise AssertionError("%s failed: %s" % (tool, result.stderr))
        out.append(result.stdout.splitlines())
    return out


class MatchesPt2Clone(unittest.TestCase):
    def test_every_generated_song_matches(self):
        for name, make in modgen.GENERATED.items():
            with self.subTest(song=name):
                theirs, ours = logs_for(make())
                self.assertGreater(len(theirs), 20, "pt2-clone log too short")
                diff = first_difference(theirs, ours)
                self.assertIsNone(diff, "\n%s: %s" % (name, diff))


    def test_every_real_song_matches(self):
        """Real MOD files in tests/songs (supplied by the project owner)."""
        names = sorted(n for n in os.listdir(SONGS) if n.lower().endswith(".mod")) \
            if os.path.isdir(SONGS) else []
        if not names:
            self.skipTest("no MOD files in tests/songs")
        for name in names:
            with self.subTest(song=name):
                with open(os.path.join(SONGS, name), "rb") as f:
                    theirs, ours = logs_for(f.read(), REAL_TICKS)
                diff = first_difference(theirs, ours)
                self.assertIsNone(diff, "\n%s: %s" % (name, diff))


class MismatchReport(unittest.TestCase):
    THEIRS = ["tick 1 pos 0 row 0 bpm 125", "  AUD0PER 428",
              "tick 7 pos 2 row 5 bpm 125", "  AUD0VOL 48", "  AUD2PER 214"]

    def test_identical_logs_have_no_difference(self):
        self.assertIsNone(first_difference(self.THEIRS, list(self.THEIRS)))

    def test_names_tick_position_row_voice_register_and_both_values(self):
        ours = list(self.THEIRS)
        ours[4] = "  AUD2PER 202"
        report = first_difference(self.THEIRS, ours)
        for part in ("tick 7", "position 2", "row 5", "voice 3", "PER",
                     "pt2-clone 214", "ours 202"):
            self.assertIn(part, report)

    def test_reports_a_missing_or_extra_write(self):
        report = first_difference(self.THEIRS, self.THEIRS[:4])
        self.assertIn("tick 7", report)
        self.assertIn("AUD2PER 214", report)
        self.assertIn("ours: (nothing)", report)


if __name__ == "__main__":
    unittest.main()
