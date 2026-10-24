"""The render command, tested end to end: MOD file in, WAV file or error out."""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modgen  # noqa: E402

RENDER = os.path.join(ROOT, "build", "render")
PAL_CCK_HZ = 3546895


def render(mod_bytes, *args):
    """Writes mod_bytes to a file, renders it, returns (result, wav_path)."""
    tmp = tempfile.mkdtemp()
    mod_path = os.path.join(tmp, "in.mod")
    wav_path = os.path.join(tmp, "out.wav")
    with open(mod_path, "wb") as f:
        f.write(mod_bytes)
    result = subprocess.run([RENDER, mod_path, wav_path, *args],
                            capture_output=True, text=True)
    return result, wav_path


def read_wav(path):
    with wave.open(path, "rb") as w:
        params = (w.getnchannels(), w.getsampwidth(), w.getframerate())
        frames = w.readframes(w.getnframes())
    samples = struct.unpack("<%dh" % (len(frames) // 2), frames)
    return params, samples[0::2], samples[1::2]


def zero_crossings(samples):
    return sum(1 for a, b in zip(samples, samples[1:]) if (a < 0) != (b < 0))


class RenderCommand(unittest.TestCase):
    def test_renders_the_plain_notes_song_to_a_stereo_48khz_wav(self):
        result, wav = render(modgen.plain_notes_song(), "--seconds", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        params, left, right = read_wav(wav)
        self.assertEqual(params, (2, 2, 48000))
        self.assertEqual(len(left), 2 * 48000)
        self.assertGreater(max(left), 1000, "channel 1 plays on the left")
        self.assertGreater(max(right), 1000, "channel 2 plays on the right")

    def test_a_note_plays_at_the_pitch_its_period_gives(self):
        # One 32-byte sine cycle, looped, at period 428: 3546895/428/32 Hz.
        song = modgen.Song()
        song.sample(1, modgen.sine_cycle(32), volume=64, loop=(0, 32))
        song.note(0, 0, 0, period=428, sample=1)
        result, wav = render(song.build(), "--seconds", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        _, left, _ = read_wav(wav)
        expected_hz = PAL_CCK_HZ / 428 / 32
        measured_hz = zero_crossings(left[4800:]) / 2 / 0.9
        self.assertAlmostEqual(measured_hz, expected_hz, delta=expected_hz * 0.02)

    def test_an_empty_song_renders_silence(self):
        result, wav = render(modgen.Song().build(), "--seconds", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        _, left, right = read_wav(wav)
        self.assertEqual(set(left) | set(right), {0})

    def test_play_once_stops_at_the_end_of_the_song(self):
        # One Pattern at speed 6 and 125 BPM: 384 ticks of 70940 colour clocks.
        song = modgen.Song()
        song.sample(1, modgen.sine_cycle(32), volume=64, loop=(0, 32))
        song.note(0, 0, 0, period=428, sample=1)
        result, wav = render(song.build(), "--play-once")
        self.assertEqual(result.returncode, 0, result.stderr)
        _, left, _ = read_wav(wav)
        expected = 384 * 70940 * 48000 / PAL_CCK_HZ
        self.assertAlmostEqual(len(left), expected, delta=1)

    def test_play_once_stops_at_f00(self):
        song = modgen.Song()
        song.note(0, 2, 0, effect=0xF, param=0)   # read on tick 13, which is the last
        result, wav = render(song.build(), "--play-once")
        self.assertEqual(result.returncode, 0, result.stderr)
        _, left, _ = read_wav(wav)
        self.assertAlmostEqual(len(left), 13 * 70940 * 48000 / PAL_CCK_HZ, delta=1)

    def high_tone_song(self, led_filter=False):
        """Channel 1 (left) plays an 8-step sine at period 113: 3923 Hz."""
        song = modgen.Song()
        song.sample(1, modgen.sine_cycle(8), volume=64, loop=(0, 8))
        song.note(0, 0, 0, period=113, sample=1, effect=0xE, param=0 if led_filter else 1)
        return song.build()

    def peak(self, *args, song=None):
        result, wav = render(song or self.high_tone_song(), "--seconds", "1", *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        _, left, right = read_wav(wav)
        return max(map(abs, left[4800:])), max(map(abs, right[4800:]))

    def test_blend_puts_some_of_the_left_channel_on_the_right(self):
        self.assertEqual(self.peak()[1], 0)
        left, right = self.peak("--blend")
        self.assertAlmostEqual(right / left, 1 / 3, delta=0.02)

    def test_the_a500_filter_softens_high_notes(self):
        plain, _ = self.peak()
        filtered, _ = self.peak("--a500")
        self.assertLess(filtered, plain * 0.85)

    def test_the_songs_led_filter_command_works_unless_no_led(self):
        plain, _ = self.peak()
        with_led, _ = self.peak(song=self.high_tone_song(led_filter=True))
        ignored, _ = self.peak("--no-led", song=self.high_tone_song(led_filter=True))
        self.assertLess(with_led, plain * 0.75)
        self.assertEqual(ignored, plain)

    def test_rejects_mod_files_with_more_than_four_channels(self):
        result, _ = render(modgen.Song(tag=b"8CHN").build())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("4-channel", result.stderr)

    def test_rejects_files_without_a_mod_signature(self):
        result, _ = render(b"\0" * 2000)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("15-sample", result.stderr)

    def test_reports_a_missing_input_file(self):
        result = subprocess.run([RENDER, "/no/such.mod", "/tmp/x.wav"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("/no/such.mod", result.stderr)


if __name__ == "__main__":
    unittest.main()
