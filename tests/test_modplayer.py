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


if __name__ == "__main__":
    unittest.main()
