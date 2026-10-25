"""The simulated chip, driven over its serial port with the real protocol
encoder (tools/modplayer.py)."""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modgen  # noqa: E402
import modplayer  # noqa: E402

SOC_TB = os.path.join(ROOT, "build", "soc_tb")


def run_soc(expect, send=b"", preload=None, switches=0, max_cycles=60000000):
    tmp = tempfile.mkdtemp()
    args = [SOC_TB, "--after", "ready", "--max", str(max_cycles), "--switches", str(switches)]
    if send:
        path = os.path.join(tmp, "send.bin")
        with open(path, "wb") as f:
            f.write(send)
        args += ["--send", path]
    if preload:
        path = os.path.join(tmp, "preload.mod")
        with open(path, "wb") as f:
            f.write(preload)
        args += ["--preload", path]
    result = subprocess.run(args + [expect], capture_output=True, text=True)
    return result.returncode, result.stdout


class SerialCommands(unittest.TestCase):
    def test_version(self):
        rc, out = run_soc("OK mod-player", modplayer.encode_simple("version"))
        self.assertEqual(rc, 0, out)

    def test_a_song_sent_over_serial_loads_and_is_named(self):
        song = modgen.plain_notes_song()
        rc, out = run_soc("OK plain notes", modplayer.encode_load(song))
        self.assertEqual(rc, 0, out)

    def test_info_lists_the_song_and_its_samples(self):
        song = modgen.plain_notes_song()
        rc, out = run_soc("sample 3 pluck (no loop)\r\nOK",
                          modplayer.encode_load(song) + modplayer.encode_simple("info"))
        self.assertEqual(rc, 0, out)
        self.assertIn("positions 3", out)

    def test_flash_id_reads_the_flash_chips_jedec_id(self):
        rc, out = run_soc("OK flash c2 20 17", modplayer.encode_simple("flash-id"))
        self.assertEqual(rc, 0, out)
        self.assertNotIn("SPI flash:", out)        # the model saw no broken rules

    def test_a_corrupted_transfer_is_rejected(self):
        frame = bytearray(modplayer.encode_load(modgen.plain_notes_song()))
        frame[2000] ^= 0x40
        rc, out = run_soc("ERR checksum mismatch", bytes(frame))
        self.assertEqual(rc, 0, out)

    def test_a_file_that_is_not_a_mod_is_rejected_with_the_loaders_reason(self):
        rc, out = run_soc("ERR only 4-channel MOD files are supported",
                          modplayer.encode_load(modgen.Song(tag=b"8CHN").build()))
        self.assertEqual(rc, 0, out)

    def test_a_cut_off_transfer_times_out(self):
        frame = modplayer.encode_load(modgen.plain_notes_song())[:500]
        rc, out = run_soc("ERR timeout", frame, max_cycles=80000000)
        self.assertEqual(rc, 0, out)

    def test_unknown_commands_are_refused(self):
        rc, out = run_soc("ERR unknown command", b"?")
        self.assertEqual(rc, 0, out)

    def test_play_from_memory_uses_a_song_already_in_sdram(self):
        song = modgen.plain_notes_song()
        rc, out = run_soc("OK plain notes", modplayer.encode_play_from_memory(len(song)),
                          preload=song)
        self.assertEqual(rc, 0, out)


if __name__ == "__main__":
    unittest.main()
