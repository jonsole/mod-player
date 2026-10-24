"""Splits a firmware binary into four $readmemh files, one per byte lane of
the 32-bit block RAM (lane 0 = bits 7:0), padded to the RAM size.

    python3 tools/bin2lanes.py firmware.bin words out_prefix
"""
import sys


def main(argv):
    data = open(argv[1], "rb").read()
    words = int(argv[2])
    if len(data) > words * 4:
        sys.exit("firmware is %d bytes; the RAM holds %d" % (len(data), words * 4))
    data = data.ljust(words * 4, b"\0")
    for lane in range(4):
        with open("%s%d.hex" % (argv[3], lane), "w") as f:
            f.write("\n".join("%02x" % data[i * 4 + lane] for i in range(words)) + "\n")


if __name__ == "__main__":
    main(sys.argv)
