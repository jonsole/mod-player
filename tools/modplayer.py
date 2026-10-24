"""Talks to the mod-player board over USB serial (docs/protocol.md).

    python3 tools/modplayer.py [--port PORT] load song.mod
    python3 tools/modplayer.py [--port PORT] play | stop | pause | info | version

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


SIMPLE = {"version": b"V", "play": b"P", "stop": b"S", "pause": b"U", "info": b"I"}


def encode_simple(name):
    """version, play, stop, pause or info."""
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


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--port", default="/dev/ttyUSB1")
    p.add_argument("command", choices=["load"] + sorted(SIMPLE))
    p.add_argument("file", nargs="?")
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
        else:
            ser.write(SIMPLE[args.command])
            lines = read_reply(ser)
    for line in lines:
        print(line)
    return 0 if lines and lines[-1].startswith("OK") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
