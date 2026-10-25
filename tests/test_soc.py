"""The simulated chip, driven over its serial port with the real protocol
encoder (tools/modplayer.py)."""
import os
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modgen  # noqa: E402
import modplayer  # noqa: E402

SOC_TB = os.path.join(ROOT, "build", "soc_tb")


def run_soc(expect, send=b"", preload=None, switches=0, max_cycles=60000000, flash_fill=0xFF):
    tmp = tempfile.mkdtemp()
    args = [SOC_TB, "--after", "ready", "--max", str(max_cycles), "--switches", str(switches),
            "--flash-fill", str(flash_fill)]
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

    def test_data_written_to_flash_reads_back(self):
        # crosses 4 KB sectors and 256-byte pages, starting on a sector that
        # isn't a 64 KB block; the flash model checks every command's rules
        data = bytes((i * 37 + (i >> 8)) & 0xFF for i in range(9000))
        addr = 0x11000
        rc, out = run_soc("OK crc %08x" % zlib.crc32(data),
                          modplayer.encode_flash_write(addr, data) +
                          modplayer.encode_flash_crc(addr, len(data)))
        self.assertEqual(rc, 0, out)
        self.assertIn("OK written", out)
        self.assertNotIn("SPI flash:", out)

    def test_writing_over_old_data_erases_it_first(self):
        # a whole 64 KB block plus part of a sector, over a flash full of
        # zeros: programming alone can only clear bits, so this only reads
        # back if the firmware erased first
        data = bytes((i * 101) & 0xFF for i in range(65536 + 300))
        rc, out = run_soc("OK crc %08x" % zlib.crc32(data),
                          modplayer.encode_flash_write(0x20000, data) +
                          modplayer.encode_flash_crc(0x20000, len(data)),
                          max_cycles=200000000, flash_fill=0)
        self.assertEqual(rc, 0, out)
        self.assertIn("OK written", out)

    def test_a_corrupted_flash_write_is_rejected_and_the_flash_left_alone(self):
        data = bytes(range(256)) * 8
        frame = bytearray(modplayer.encode_flash_write(0x3000, data))
        frame[500] ^= 0x01
        rc, out = run_soc("OK crc %08x" % zlib.crc32(b"\xff" * len(data)),
                          bytes(frame) + modplayer.encode_flash_crc(0x3000, len(data)))
        self.assertEqual(rc, 0, out)
        self.assertIn("ERR checksum mismatch", out)

    def test_a_flash_write_not_on_a_4k_sector_is_refused(self):
        rc, out = run_soc("ERR flash address must be a multiple of 4096",
                          modplayer.encode_flash_write(0x1001, b"x"))
        self.assertEqual(rc, 0, out)

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
