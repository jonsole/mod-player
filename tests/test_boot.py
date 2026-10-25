"""The boot loader (issue #20, ADR-0006, ADR-0011): the simulated chip with
only the boot loader in its RAM, as the FPGA image has it. It runs the
firmware from the model flash, or firmware sent over serial."""
import os
import re
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modplayer  # noqa: E402

BOOT_TB = os.path.join(ROOT, "build", "soc_boot_tb")
FIRMWARE = os.path.join(ROOT, "build", "fw", "firmware.bin")


def firmware():
    with open(FIRMWARE, "rb") as f:
        return f.read()


def flash_with_firmware(record):
    """A flash image file: erased, with `record` in the firmware region."""
    path = os.path.join(tempfile.mkdtemp(), "flash.bin")
    with open(path, "wb") as f:
        f.write(b"\xff" * modplayer.FIRMWARE_ADDR + record)
    return path


def run_boot(expect, after=None, send=b"", flash=None, max_cycles=60000000, steps=()):
    """Runs the boot testbench. Each step waits for its text on the serial
    output (after the previous step), then sends its bytes; `expect` must
    then appear after the last step."""
    args = [BOOT_TB, "--max", str(max_cycles)]
    if flash:
        args += ["--flash-image", flash]
    steps = list(steps) + ([(after, send)] if after else [])
    tmp = tempfile.mkdtemp()
    for i, (text, data) in enumerate(steps):
        path = os.path.join(tmp, "send%d.bin" % i)
        with open(path, "wb") as f:
            f.write(data)
        args += ["--after", text, "--send", path]
    result = subprocess.run(args + [expect], capture_output=True, text=True)
    return result.returncode, result.stdout


class BootLoader(unittest.TestCase):
    def test_firmware_in_flash_runs_at_power_on(self):
        rc, out = run_boot("ready", flash=flash_with_firmware(modplayer.firmware_record(firmware())))
        self.assertEqual(rc, 0, out)
        self.assertIn("boot loader", out)
        self.assertIn("firmware from flash", out)
        self.assertIn("sdram ok", out)

    def test_with_no_firmware_in_flash_it_waits_for_firmware_over_serial(self):
        rc, out = run_boot("ready", after="no firmware in flash: waiting for serial",
                           send=modplayer.encode_run_firmware(firmware()))
        self.assertEqual(rc, 0, out)
        self.assertIn("OK running", out)

    def test_damaged_firmware_in_flash_is_not_run(self):
        record = bytearray(modplayer.firmware_record(firmware()))
        record[200] ^= 0x04
        rc, out = run_boot("ready", after="firmware in flash damaged: waiting for serial",
                           send=modplayer.encode_run_firmware(firmware()),
                           flash=flash_with_firmware(bytes(record)))
        self.assertEqual(rc, 0, out)
        self.assertNotIn("firmware from flash", out)

    def test_the_pc_can_claim_the_board_before_flash_firmware_starts(self):
        # a byte inside the power-on window keeps the board in the boot loader
        rc, out = run_boot("ready", after="boot loader",
                           send=modplayer.encode_simple("boot") + modplayer.encode_run_firmware(firmware()),
                           flash=flash_with_firmware(modplayer.firmware_record(firmware())))
        self.assertEqual(rc, 0, out)
        self.assertIn("OK boot loader", out)
        self.assertNotIn("firmware from flash", out)

    def test_running_firmware_returns_to_the_boot_loader_on_b(self):
        rc, out = run_boot("OK boot loader", after="ready", send=modplayer.encode_simple("boot"),
                           flash=flash_with_firmware(modplayer.firmware_record(firmware())))
        self.assertEqual(rc, 0, out)

    def test_firmware_sent_after_returning_to_the_boot_loader_runs(self):
        # B, then new firmware: the board comes up a second time
        rc, out = run_boot("ready", steps=[("ready", modplayer.encode_simple("boot")),
                                           ("OK boot loader", modplayer.encode_run_firmware(firmware()))],
                           flash=flash_with_firmware(modplayer.firmware_record(firmware())),
                           max_cycles=80000000)
        self.assertEqual(rc, 0, out)
        self.assertEqual(out.count("sdram ok"), 2, out)

    def test_a_corrupted_firmware_transfer_is_refused(self):
        frame = bytearray(modplayer.encode_run_firmware(firmware()))
        frame[300] ^= 0x01
        rc, out = run_boot("ERR checksum mismatch", after="waiting for serial", send=bytes(frame))
        self.assertEqual(rc, 0, out)

    def test_firmware_too_large_for_its_ram_is_refused(self):
        rc, out = run_boot("ERR firmware too large", after="waiting for serial",
                           send=modplayer.encode_run_firmware(bytes(modplayer.FIRMWARE_MAX + 4)))
        self.assertEqual(rc, 0, out)

    def test_the_boot_loader_answers_version(self):
        rc, out = run_boot("OK boot loader", after="waiting for serial",
                           send=modplayer.encode_simple("version"))
        self.assertEqual(rc, 0, out)

    def test_saved_firmware_is_the_record_the_boot_loader_reads(self):
        # firmware sent over serial, then saved to flash with W: the flash
        # then holds exactly the record
        record = modplayer.firmware_record(firmware())
        rc, out = run_boot("OK crc %08x" % zlib.crc32(record),
                           steps=[("waiting for serial", modplayer.encode_run_firmware(firmware())),
                                  ("ready", modplayer.encode_save_firmware(firmware()) +
                                   modplayer.encode_flash_crc(modplayer.FIRMWARE_ADDR, len(record)))],
                           max_cycles=200000000)
        self.assertEqual(rc, 0, out)
        self.assertIn("OK written", out)


class Layout(unittest.TestCase):
    """The firmware, boot loader and PC script agree on where things go."""

    def header(self, name):
        with open(os.path.join(ROOT, "firmware", name)) as f:
            return f.read()

    def define(self, text, name):
        return int(re.search(r"#define %s\s+(0x[0-9a-fA-F]+)u" % name, text).group(1), 16)

    def test_flash_addresses_agree(self):
        text = self.header("flash.h")
        self.assertEqual(self.define(text, "FLASH_FW_ADDR"), modplayer.FIRMWARE_ADDR)
        self.assertEqual(self.define(text, "FLASH_FW_MAGIC").to_bytes(4, "little"), modplayer.FIRMWARE_MAGIC)

    def test_firmware_fits_below_the_boot_loader(self):
        text = self.header("hw.h")
        self.assertEqual(self.define(text, "BOOT_BASE"), modplayer.FIRMWARE_MAX)
        self.assertLessEqual(len(firmware()), modplayer.FIRMWARE_MAX)

    def test_the_firmware_record_is_header_then_image(self):
        image = bytes(range(256)) * 3
        record = modplayer.firmware_record(image)
        self.assertEqual(record[:4], b"MODF")
        self.assertEqual(int.from_bytes(record[4:8], "little"), len(image))
        self.assertEqual(int.from_bytes(record[8:12], "little"), zlib.crc32(image))
        self.assertEqual(record[12:16], bytes(4))
        self.assertEqual(record[16:], image)


if __name__ == "__main__":
    unittest.main()
