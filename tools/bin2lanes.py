"""Splits a firmware binary into four $readmemh files, one per byte lane of
the 32-bit block RAM (lane 0 = bits 7:0), padded with zeros to the RAM size.
With an offset, the binary goes at that byte address (the boot loader goes
at 0xF000).

    python3 tools/bin2lanes.py firmware.bin words out_prefix [offset]
"""
import sys


def main(argv):
    data = open(argv[1], "rb").read()
    words = int(argv[2])
    offset = int(argv[4], 0) if len(argv) > 4 else 0
    if offset % 4 or offset + len(data) > words * 4:
        sys.exit("%d bytes at 0x%x don't fit the RAM's %d" % (len(data), offset, words * 4))
    data = bytes(offset) + data
    data = data.ljust(words * 4, b"\0")
    for lane in range(4):
        with open("%s%d.hex" % (argv[3], lane), "w") as f:
            f.write("\n".join("%02x" % data[i * 4 + lane] for i in range(words)) + "\n")


if __name__ == "__main__":
    main(sys.argv)
