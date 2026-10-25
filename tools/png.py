"""Just enough PNG for the console's reference images: 8-bit RGB, no
interlacing. Python's standard library only.

    python3 tools/png.py in.ppm out.png      (binary PPM to PNG)
"""
import struct
import sys
import zlib


def write_png(path, width, height, rgb):
    """rgb: width * height * 3 bytes, row by row."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def read_png(path):
    """Returns (width, height, rgb bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)
    pos, idat, width = 8, b"", None
    while pos < len(data):
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour != 2 or interlace:
                raise ValueError("%s: only 8-bit RGB without interlacing is supported" % path)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride, out, prev = zlib.decompress(idat), width * 3, bytearray(), bytearray(width * 3)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b, c = prev[i], prev[i - 3] if i >= 3 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return width, height, bytes(out)


def read_ppm(path):
    """Binary PPM (P6, maxval 255) as written by console_tb: (width, height, rgb)."""
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6" or parts[2] != b"255":
        raise ValueError("%s is not a binary PPM" % path)
    width, height = map(int, parts[1].split())
    return width, height, parts[3][:width * height * 3]


if __name__ == "__main__":
    write_png(sys.argv[2], *read_ppm(sys.argv[1]))
