"""Builds 4-channel, 31-sample MOD files for tests.

    song = Song()
    song.sample(1, data, volume=64, loop=(start, length))  # bytes
    song.note(pattern, row, channel, period=428, sample=1, effect=0xF, param=3)
    open("x.mod", "wb").write(song.build())

Run as a script to write the generated test Songs to a folder.
"""
import math
import os
import struct
import sys


class Song:
    def __init__(self, name="test song", tag=b"M.K."):
        self.name = name
        self.tag = tag
        self.samples = {}      # number -> (data, volume, finetune, loop)
        self.cells = {}        # (pattern, row, channel) -> 4 bytes
        self.positions = [0]

    def sample(self, number, data, volume=64, finetune=0, loop=None, name=""):
        """data: signed bytes (ints -128..127); loop: (start, length) in bytes, even."""
        if len(data) % 2:
            data = list(data) + [0]
        self.samples[number] = (bytes(b & 0xFF for b in data), volume, finetune, loop, name)

    def note(self, pattern, row, channel, period=0, sample=0, effect=0, param=0):
        self.cells[(pattern, row, channel)] = bytes([
            (sample & 0xF0) | ((period >> 8) & 0x0F),
            period & 0xFF,
            ((sample & 0x0F) << 4) | (effect & 0x0F),
            param & 0xFF,
        ])

    def build(self):
        num_patterns = max([p for p, _, _ in self.cells] + self.positions) + 1
        out = bytearray(self.name.encode()[:20].ljust(20, b"\0"))
        for n in range(1, 32):
            data, volume, finetune, loop, name = self.samples.get(
                n, (b"", 0, 0, None, ""))
            start, length = loop if loop else (0, 2)
            out += name.encode()[:22].ljust(22, b"\0")
            out += struct.pack(">HBBHH", len(data) // 2, finetune & 0xF, volume,
                               start // 2, length // 2)
        out += bytes([len(self.positions), 127])
        out += bytes(self.positions).ljust(128, b"\0")
        out += self.tag
        for p in range(num_patterns):
            for row in range(64):
                for ch in range(4):
                    out += self.cells.get((p, row, ch), b"\0\0\0\0")
        for n in range(1, 32):
            if n in self.samples:
                out += self.samples[n][0]
        return bytes(out)


def sine_cycle(length, amplitude=100):
    return [round(amplitude * math.sin(2 * math.pi * i / length)) for i in range(length)]


def square_cycle(length, amplitude=100):
    return [amplitude if i < length // 2 else -amplitude for i in range(length)]


# ProTracker periods for octaves 1-3, finetune 0.
C1, D1, E1, F1, G1, A1, B1 = 856, 762, 678, 640, 570, 508, 453
C2, D2, E2, F2, G2, A2, B2 = 428, 381, 339, 320, 285, 254, 226
C3, E3, G3 = 214, 170, 143


def plain_notes_song():
    """Notes, Sample changes and speed changes only: no other effects."""
    song = Song("plain notes")
    song.sample(1, sine_cycle(32) * 4, volume=64, loop=(0, 128), name="sine")
    song.sample(2, square_cycle(16) * 8, volume=40, loop=(0, 128), name="square")
    song.sample(3, [round(100 * math.sin(i / 3)) * (300 - i) // 300 for i in range(300)],
                volume=56, name="pluck (no loop)")
    melody = [C2, D2, E2, F2, G2, A2, B2, C3]
    for i, period in enumerate(melody):
        song.note(0, i * 8, 0, period=period, sample=1)
        song.note(0, i * 8 + 4, 1, period=period, sample=2)
    for row in range(0, 64, 16):
        song.note(0, row, 2, period=C1, sample=3)
    song.note(0, 32, 3, effect=0xF, param=3)   # faster from row 32
    song.note(1, 0, 3, effect=0xF, param=6)    # back to normal
    for i, period in enumerate(reversed(melody)):
        song.note(1, i * 8, 0, period=period, sample=2)   # Sample change on channel 1
        song.note(1, i * 8 + 2, 1, period=period, sample=1)
        song.note(1, i * 8 + 4, 2, period=period, sample=3)
    song.positions = [0, 1, 0]
    return song.build()


GENERATED = {
    "plain-notes.mod": plain_notes_song,
}


def main(argv):
    folder = argv[1] if len(argv) > 1 else "."
    os.makedirs(folder, exist_ok=True)
    for name, make in GENERATED.items():
        with open(os.path.join(folder, name), "wb") as f:
            f.write(make())
        print(os.path.join(folder, name))


if __name__ == "__main__":
    main(sys.argv)
