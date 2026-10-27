"""The microSD card (issue #25): the simulated chip, driven over its serial
port, against a rule-checking model of an SD card in SPI mode
(hw/sim/spi_sd_model.h). The model fails the run if the firmware breaks a
rule: fewer than 74 start-up clocks, SCK faster than 400 kHz before the card
is ready, commands out of order."""
import os
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modplayer  # noqa: E402

SOC_TB = os.path.join(ROOT, "build", "soc_tb")
BLOCKS = 40


def card_image(blocks=BLOCKS):
    """Blocks that differ from each other and from block to block."""
    return bytes((i * 37 + (i >> 9) * 11 + (i >> 5)) & 0xFF for i in range(blocks * 512))


def run_card(expect, send, image=None, card_args=(), max_cycles=200000000):
    tmp = tempfile.mkdtemp()
    args = [SOC_TB, "--after", "ready", "--max", str(max_cycles)]
    if image is not None:
        path = os.path.join(tmp, "card.img")
        with open(path, "wb") as f:
            f.write(image)
        args += ["--sd-image", path] + list(card_args)
    path = os.path.join(tmp, "send.bin")
    with open(path, "wb") as f:
        f.write(send)
    args += ["--send", path, expect]
    result = subprocess.run(args, capture_output=True, text=True)
    return result.returncode, result.stdout


class SdCard(unittest.TestCase):
    def test_an_sdhc_card_starts_and_a_block_reads_back(self):
        image = card_image()
        rc, out = run_card("OK crc %08x" % zlib.crc32(image[512 * 3:512 * 4]),
                           modplayer.encode_simple("sd-init") + modplayer.encode_sd_crc(3, 1), image)
        self.assertEqual(rc, 0, out)
        self.assertIn("OK sd sdhc", out)
        self.assertNotIn("SD card:", out)           # the model saw no broken rules

    def test_many_blocks_read_in_order(self):
        image = card_image()
        rc, out = run_card("OK crc %08x" % zlib.crc32(image[512 * 5:512 * 25]),
                           modplayer.encode_simple("sd-init") + modplayer.encode_sd_crc(5, 20), image)
        self.assertEqual(rc, 0, out)

    def test_the_last_block_reads_and_the_one_after_it_does_not(self):
        image = card_image()
        rc, out = run_card("OK crc %08x" % zlib.crc32(image[-512:]),
                           modplayer.encode_simple("sd-init") + modplayer.encode_sd_crc(BLOCKS - 1, 1), image)
        self.assertEqual(rc, 0, out)
        rc, out = run_card("ERR SD read failed",
                           modplayer.encode_simple("sd-init") + modplayer.encode_sd_crc(BLOCKS, 1), image)
        self.assertEqual(rc, 0, out)

    def test_a_standard_capacity_card_is_addressed_by_byte(self):
        image = card_image()
        rc, out = run_card("OK crc %08x" % zlib.crc32(image[512 * 7:512 * 9]),
                           modplayer.encode_simple("sd-init") + modplayer.encode_sd_crc(7, 2), image,
                           ["--sd-sdsc"])
        self.assertEqual(rc, 0, out)
        self.assertIn("OK sd sdsc", out)

    def test_a_version_1_card_starts_without_cmd8(self):
        image = card_image()
        rc, out = run_card("OK crc %08x" % zlib.crc32(image[:512]),
                           modplayer.encode_simple("sd-init") + modplayer.encode_sd_crc(0, 1), image,
                           ["--sd-v1"])
        self.assertEqual(rc, 0, out)
        self.assertIn("OK sd sdsc", out)

    def test_a_card_that_takes_a_long_time_to_start_is_waited_for(self):
        image = card_image()
        rc, out = run_card("OK crc %08x" % zlib.crc32(image[:512]),
                           modplayer.encode_simple("sd-init") + modplayer.encode_sd_crc(0, 1), image,
                           ["--sd-polls", "60"])
        self.assertEqual(rc, 0, out)

    def test_no_card_gives_an_error(self):
        rc, out = run_card("ERR no SD card", modplayer.encode_simple("sd-init"))
        self.assertEqual(rc, 0, out)

    def test_a_card_that_never_becomes_ready_gives_an_error(self):
        rc, out = run_card("ERR SD card doesn't start up", modplayer.encode_simple("sd-init"), card_image(),
                           ["--sd-polls", "100000000"], max_cycles=400000000)
        self.assertEqual(rc, 0, out)

    def test_reading_before_starting_the_card_is_refused_without_touching_it(self):
        rc, out = run_card("ERR SD card not started", modplayer.encode_sd_crc(0, 1), card_image())
        self.assertEqual(rc, 0, out)                # the model saw no command


if __name__ == "__main__":
    unittest.main()
