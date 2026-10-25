"""The PC script's file handling (tools/modplayer.py)."""
import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modplayer  # noqa: E402


def fake_bit(payload):
    """A Xilinx .bit file as bitgen writes one: a fixed preamble, then
    fields a (design), b (part), c (date), d (time) with 16-bit lengths, and
    e (the configuration data) with a 32-bit length."""
    out = struct.pack(">H", 9) + bytes.fromhex("0ff00ff00ff00ff000") + struct.pack(">H", 1)
    for key, text in ((b"a", b"top.ncd;UserID=0xFFFFFFFF\0"), (b"b", b"6slx25ftg256\0"),
                      (b"c", b"2026/10/25\0"), (b"d", b"13:34:40\0")):
        out += key + struct.pack(">H", len(text)) + text
    return out + b"e" + struct.pack(">I", len(payload)) + payload


class BitFiles(unittest.TestCase):
    def test_the_configuration_data_is_what_goes_to_flash(self):
        payload = b"\xff" * 16 + b"\xaa\x99\x55\x66" + bytes(range(200))
        image, part = modplayer.bit_to_flash_image(fake_bit(payload))
        self.assertEqual(image, payload)
        self.assertEqual(part, "6slx25ftg256")

    def test_a_file_that_is_not_a_bitstream_is_refused(self):
        with self.assertRaises(ValueError):
            modplayer.bit_to_flash_image(b"not a bitstream at all")

    def test_a_truncated_bitstream_is_refused(self):
        with self.assertRaises(ValueError):
            modplayer.bit_to_flash_image(fake_bit(bytes(1000))[:-10])

    def test_flash_write_frames_address_length_data_and_crc(self):
        frame = modplayer.encode_flash_write(0x1000, b"abc")
        self.assertEqual(frame[:9], b"W" + struct.pack("<II", 0x1000, 3))
        self.assertEqual(frame[9:12], b"abc")
        self.assertEqual(len(frame), 16)


class FakeBoard:
    """Enough of a serial port and a board for run_firmware: the firmware
    (or a boot loader that only starts listening after `deaf` writes)
    answers B, X and W as docs/protocol.md says."""

    def __init__(self, deaf=0, busy_after_ready=0):
        # busy_after_ready: writes lost after start-up, as while the
        # firmware loads a saved Song from flash
        self.deaf, self.out, self.written = deaf, b"", []
        self.busy_after_ready, self.busy = busy_after_ready, 0

    @property
    def in_waiting(self):
        return len(self.out)

    def read(self, n):
        data, self.out = self.out[:n], self.out[n:]
        return data

    def reset_input_buffer(self):
        self.out = b""

    def write(self, data):
        self.written.append(data)
        if self.deaf:
            self.deaf -= 1
        elif self.busy:
            self.busy -= 1
        elif data == b"B":
            self.out += b"OK boot loader 0.1\r\n"
        elif data == b"V":
            self.out += b"OK mod-player 0.1\r\n"
        elif data[:1] == b"X":
            self.out += b"OK running\r\nmod-player 0.1\r\nsdram ok\r\npaula ok\r\nready\r\n"
            self.busy = self.busy_after_ready
        elif data[:1] == b"W":
            self.out += b"OK written\r\n"


class Firmware(unittest.TestCase):
    def test_firmware_is_sent_to_the_boot_loader_and_saved(self):
        board = FakeBoard()
        image = bytes(range(100))
        lines = modplayer.run_firmware(board, image, save=True)
        self.assertEqual(lines[-1], "OK written")
        sent = [w for w in board.written if w not in (b"B", b"V")]
        self.assertEqual(sent, [modplayer.encode_run_firmware(image), modplayer.encode_save_firmware(image)])

    def test_b_is_repeated_until_the_boot_loader_listens(self):
        # e.g. broken firmware, then the FPGA reloaded over JTAG
        board = FakeBoard(deaf=5)
        lines = modplayer.run_firmware(board, b"\x13" * 8)
        self.assertTrue(lines[-1].startswith("OK running"), lines)
        self.assertGreater(board.written.count(b"B"), 5)

    def test_saving_waits_until_the_firmware_answers(self):
        # found on the board: a saved Song loading after "ready" ate the W
        board = FakeBoard(busy_after_ready=2)
        lines = modplayer.run_firmware(board, b"\x13" * 8, save=True)
        self.assertEqual(lines[-1], "OK written", lines)
        self.assertEqual(board.written[-1][:1], b"W")
        self.assertIn(b"V", board.written)

    def test_without_save_nothing_is_written_to_flash(self):
        board = FakeBoard()
        modplayer.run_firmware(board, b"\x13" * 8)
        self.assertFalse(any(w[:1] == b"W" for w in board.written))


if __name__ == "__main__":
    unittest.main()
