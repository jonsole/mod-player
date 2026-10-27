"""Talks to the mod-player board over USB serial (docs/protocol.md).

    python3 tools/modplayer.py [--port PORT] load song.mod
    python3 tools/modplayer.py [--port PORT] play | stop | pause | info | version | flash-id
    python3 tools/modplayer.py [--port PORT] flash-write top.bit   (the FPGA image, to boot from)
    python3 tools/modplayer.py [--port PORT] save song.mod | forget  (the Song played at power-on)
    python3 tools/modplayer.py [--port PORT] firmware firmware.bin [--save]
                                     (run new firmware now; --save also keeps it in flash)
    python3 tools/modplayer.py [--port PORT] boot   (back to the boot loader)
    python3 tools/modplayer.py [--port PORT] sd-init | sd-crc [--lba N] [--count N]   (the microSD card)
    python3 tools/modplayer.py [--port PORT] card-ls | card-play NAME | card-crc NAME  (Songs on the card)

PORT defaults to /dev/ttyUSB1 (the FT2232H's second port on Linux; on
Windows something like COM4). Needs pyserial (pip install pyserial).
The encode_* functions are also used by the simulation tests.
"""
import argparse
import struct
import sys
import time
import zlib

BAUD = 1000000


def encode_load(data):
    """Load and play: L, length, the file, CRC-32."""
    return b"L" + struct.pack("<I", len(data)) + data + struct.pack("<I", zlib.crc32(data))


def encode_play_from_memory(length):
    """Play a MOD file already at the start of SDRAM."""
    return b"M" + struct.pack("<I", length)


SIMPLE = {"version": b"V", "play": b"P", "stop": b"S", "pause": b"U", "info": b"I",
          "flash-id": b"F", "boot": b"B", "sd-init": b"D", "card-ls": b"N"}


def encode_name(cmd, name):
    """A card command with a file name: the command byte, the name's length (one byte), the name."""
    raw = name.encode("ascii")
    if not 0 < len(raw) < 256:
        raise ValueError("a file name is 1 to 255 ASCII characters")
    return cmd + bytes([len(raw)]) + raw


def encode_card_crc(name):
    """The size and CRC-32 of a file on the card: K, name."""
    return encode_name(b"K", name)


def encode_card_play(name):
    """Load a Song from the card and play it: R, name."""
    return encode_name(b"R", name)


def encode_sd_crc(lba, count):
    """CRC-32 of `count` 512-byte blocks of the SD card from block `lba`: E, lba, count."""
    return b"E" + struct.pack("<II", lba, count)


def encode_flash_write(addr, data):
    """Write to flash: W, address, length, the data, CRC-32. The board
    erases what it needs (whole 4 KB sectors from addr), programs and
    verifies."""
    return b"W" + struct.pack("<II", addr, len(data)) + data + struct.pack("<I", zlib.crc32(data))


# Flash layout (docs/flash-layout.md; firmware/flash.h has the same numbers)
SAVED_SONG_ADDR = 0x200000
SAVED_SONG_SIZE = 0x600000
SAVED_SONG_MAGIC = b"MODS"


def saved_song_record(song):
    """What goes in the saved-Song region: MODS, length, CRC-32, 4 zero
    bytes, then the MOD file."""
    if 16 + len(song) > SAVED_SONG_SIZE:
        raise ValueError("Song too large to save (%d bytes; at most %d)" % (len(song), SAVED_SONG_SIZE - 16))
    return SAVED_SONG_MAGIC + struct.pack("<II", len(song), zlib.crc32(song)) + bytes(4) + song


def encode_save(song):
    """Save a Song to flash, to play at power-on."""
    return encode_flash_write(SAVED_SONG_ADDR, saved_song_record(song))


def encode_forget():
    """Remove the saved Song (its header becomes zeros)."""
    return encode_flash_write(SAVED_SONG_ADDR, bytes(16))


# The firmware the boot loader runs (ADR-0011; firmware/flash.h, firmware/hw.h)
FIRMWARE_ADDR = 0x100000
FIRMWARE_MAGIC = b"MODF"
FIRMWARE_MAX = 0xF000          # the firmware RAM below the boot loader


def firmware_record(image):
    """What goes in the firmware region of flash: MODF, length, CRC-32,
    4 zero bytes, then the firmware image (linked to run at address 0)."""
    if len(image) > FIRMWARE_MAX:
        raise ValueError("firmware too large (%d bytes; at most %d)" % (len(image), FIRMWARE_MAX))
    return FIRMWARE_MAGIC + struct.pack("<II", len(image), zlib.crc32(image)) + bytes(4) + image


def encode_run_firmware(image):
    """To the boot loader: X, length, the firmware image, CRC-32. It loads
    the image at address 0 and runs it."""
    return b"X" + struct.pack("<I", len(image)) + image + struct.pack("<I", zlib.crc32(image))


def encode_save_firmware(image):
    """To the firmware: write a firmware record to flash for the boot loader."""
    return encode_flash_write(FIRMWARE_ADDR, firmware_record(image))


def encode_text(row, col, attr, text):
    """Put text on the HDMI text console: T, row, column, attribute,
    length, then the characters (code page 437)."""
    if len(text) > 255:
        raise ValueError("at most 255 characters at a time")
    return b"T" + bytes([row, col, attr, len(text)]) + text


def encode_flash_crc(addr, length):
    """CRC-32 of a flash range: C, address, length."""
    return b"C" + struct.pack("<II", addr, length)


def bit_to_flash_image(bit):
    """Splits a Xilinx .bit file into (configuration data, part name). The
    configuration data is what the FPGA reads from SPI flash at power-on."""
    if len(bit) < 13 or bit[:2] != b"\x00\x09" or bit[11:13] != b"\x00\x01":
        raise ValueError("not a Xilinx .bit file")
    pos, part = 13, ""
    while pos < len(bit):
        key = bit[pos:pos + 1]
        if key in (b"a", b"b", b"c", b"d"):
            (n,) = struct.unpack(">H", bit[pos + 1:pos + 3])
            if key == b"b":
                part = bit[pos + 3:pos + 3 + n].rstrip(b"\0").decode("latin-1")
            pos += 3 + n
        elif key == b"e":
            (n,) = struct.unpack(">I", bit[pos + 1:pos + 5])
            data = bit[pos + 5:pos + 5 + n]
            if len(data) != n:
                raise ValueError("truncated .bit file")
            return data, part
        else:
            raise ValueError("unexpected field %r in .bit file" % key)
    raise ValueError("no configuration data in .bit file")


def encode_simple(name):
    """version, play, stop, pause, info, flash-id or boot."""
    return SIMPLE[name]


def read_reply(ser, timeout=10.0):
    """Returns the reply lines, up to and including the OK/ERR line."""
    lines, buf, deadline = [], b"", time.time() + timeout
    while time.time() < deadline:
        buf += ser.read(ser.in_waiting or 1)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("latin-1").rstrip("\r")
            lines.append(text)
            if text.startswith("OK") or text.startswith("ERR"):
                return lines
    raise TimeoutError("no reply from the board (lines so far: %r)" % lines)


def read_until(ser, wanted, timeout=10.0):
    """Returns the lines up to and including the one that is `wanted`, or
    up to an ERR or FAIL line."""
    lines, buf, deadline = [], b"", time.time() + timeout
    while time.time() < deadline:
        buf += ser.read(ser.in_waiting or 1)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("latin-1").rstrip("\r")
            lines.append(text)
            if text == wanted or text.startswith("ERR") or "FAIL" in text:
                return lines
    raise TimeoutError("no %r from the board (lines so far: %r)" % (wanted, lines))


def enter_boot_loader(ser, timeout=20.0):
    """Sends B every 50 ms until the boot loader answers: the running
    firmware jumps to it, and a boot loader just starting (after a JTAG
    reload, say) stays in it. Then drops the extra answers. Returns whether
    it answered."""
    buf, deadline = b"", time.time() + timeout
    while time.time() < deadline:
        ser.write(SIMPLE["boot"])
        time.sleep(0.05)
        buf += ser.read(ser.in_waiting)
        if b"OK boot loader" in buf:
            time.sleep(0.1)
            ser.reset_input_buffer()
            return True
    return False


def wait_until_listening(ser, tries=10):
    """Older firmware said "ready" before loading a saved Song from flash,
    and bytes sent meanwhile were lost; it now says it after. Sends V until
    it answers, then drops any late answer. Returns whether it answered."""
    for _ in range(tries):
        ser.write(SIMPLE["version"])
        try:
            if read_reply(ser, timeout=1.0)[-1].startswith("OK mod-player"):
                time.sleep(0.1)
                ser.reset_input_buffer()
                return True
        except TimeoutError:
            pass
    return False


def run_firmware(ser, image, save=False):
    """Back to the boot loader, send the firmware and wait for it to come
    up; then, with save, write it to flash. Returns the board's lines."""
    if not enter_boot_loader(ser):
        return ["ERR no answer from the boot loader: reload the FPGA image (build.bat prog) "
                "while this is waiting"]
    lines = ["OK boot loader"]
    print("sending firmware (%d bytes)" % len(image))
    ser.write(encode_run_firmware(image))
    # OK running, then the firmware's start-up lines (the SDRAM test takes a few seconds)
    lines += read_until(ser, "ready", timeout=len(image) * 10 / BAUD + 30)
    if lines[-1] != "ready":
        lines.append("ERR the firmware didn't start")
        return lines
    if save:
        record = firmware_record(image)
        if not wait_until_listening(ser):
            lines.append("ERR the firmware doesn't answer after start-up")
            return lines
        print("saving it to flash")
        ser.write(encode_save_firmware(image))
        lines += read_reply(ser, timeout=len(record) * 10 / BAUD + 30)
    elif lines[-1] == "ready":
        lines.append("OK running (not saved: power-off brings back the flash firmware)")
    return lines


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--port", default="/dev/ttyUSB1")
    p.add_argument("command", choices=["load", "flash-write", "save", "forget", "firmware", "sd-crc",
                                       "card-play", "card-crc"] + sorted(SIMPLE))
    p.add_argument("file", nargs="?")
    p.add_argument("--addr", type=lambda s: int(s, 0), default=0,
                   help="flash-write: flash address (default 0, where the FPGA boots from)")
    p.add_argument("--lba", type=lambda s: int(s, 0), default=0, help="sd-crc: the first block")
    p.add_argument("--count", type=lambda s: int(s, 0), default=1, help="sd-crc: how many blocks")
    p.add_argument("--save", action="store_true",
                   help="firmware: also write it to flash, for the boot loader to run at power-on")
    args = p.parse_args(argv[1:])
    try:
        import serial
    except ImportError:
        sys.exit("needs pyserial: pip install pyserial")
    with serial.Serial(args.port, BAUD, timeout=0.1) as ser:
        ser.reset_input_buffer()
        if args.command == "load":
            if not args.file:
                sys.exit("load needs a MOD file")
            data = open(args.file, "rb").read()
            print("sending %s (%d bytes, about %.1f s)" % (args.file, len(data), len(data) * 10 / BAUD))
            ser.write(encode_load(data))
            lines = read_reply(ser, timeout=len(data) * 10 / BAUD + 5)
        elif args.command == "sd-crc":
            ser.write(encode_sd_crc(args.lba, args.count))
            lines = read_reply(ser, timeout=args.count * 0.01 + 10)
        elif args.command in ("card-play", "card-crc"):
            if not args.file:
                sys.exit("%s needs the name of a file on the card" % args.command)
            ser.write((encode_card_play if args.command == "card-play" else encode_card_crc)(args.file))
            lines = read_reply(ser, timeout=60)                      # a large Song: a few seconds
        elif args.command == "card-ls":
            ser.write(SIMPLE["card-ls"])
            lines = read_reply(ser, timeout=30)
        elif args.command == "firmware":
            if not args.file:
                sys.exit("firmware needs a firmware .bin file")
            image = open(args.file, "rb").read()
            if len(image) > FIRMWARE_MAX:
                sys.exit("%s is %d bytes; the firmware RAM holds %d" % (args.file, len(image), FIRMWARE_MAX))
            lines = run_firmware(ser, image, args.save)
        elif args.command in ("save", "forget"):
            if args.command == "save":
                if not args.file:
                    sys.exit("save needs a MOD file")
                frame = encode_save(open(args.file, "rb").read())
                print("saving %s to flash: it will play at power-on" % args.file)
            else:
                frame = encode_forget()
            ser.write(frame)
            lines = read_reply(ser, timeout=len(frame) * 10 / BAUD + (len(frame) / 65536 + 1) * 4 + 30)
        elif args.command == "flash-write":
            if not args.file:
                sys.exit("flash-write needs a .bit file")
            image, part = bit_to_flash_image(open(args.file, "rb").read())
            if part and part != "6slx25ftg256":
                sys.exit("%s is for %s, not this board's 6slx25ftg256" % (args.file, part))
            print("writing %s to flash at 0x%x (%d bytes): about %.0f s"
                  % (args.file, args.addr, len(image), len(image) * 10 / BAUD + len(image) / 40000 + 5))
            ser.write(encode_flash_write(args.addr, image))
            # sending, then up to ~3 s per 64 KB erased, then programming and checking
            lines = read_reply(ser, timeout=len(image) * 10 / BAUD + (len(image) / 65536 + 1) * 4 + 60)
            if lines and lines[-1].startswith("OK"):
                ser.write(encode_flash_crc(args.addr, len(image)))
                lines += read_reply(ser, timeout=30)
                if lines[-1] != "OK crc %08x" % zlib.crc32(image):
                    lines.append("ERR flash doesn't read back as written")
        else:
            ser.write(SIMPLE[args.command])
            lines = read_reply(ser)
    for line in lines:
        print(line)
    return 0 if lines and lines[-1].startswith("OK") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
