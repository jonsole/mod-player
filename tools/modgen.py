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


def effect_song(name, cells, positions=(0,)):
    """A Song for testing effects: two looping Samples (1: sine, 2: square)
    and one plucked Sample (3), with the given cells:
    (pattern, row, channel, period, sample, effect, param)."""
    song = Song(name)
    song.sample(1, sine_cycle(32) * 4, volume=48, loop=(0, 128), name="sine")
    song.sample(2, square_cycle(16) * 8, volume=64, loop=(64, 64), name="square")
    song.sample(3, [round(100 * math.sin(i / 3)) * (600 - i) // 600 for i in range(600)],
                volume=56, finetune=0, name="pluck")
    for number, finetune in ((4, 15), (5, 7), (6, 8), (7, 1)):  # -1, +7, -8, +1
        song.sample(number, square_cycle(32) * 2, volume=50, finetune=finetune,
                    loop=(0, 64), name="finetune %d" % finetune)
    sweep = [round(100 * math.sin(i * i / 20000)) for i in range(4096)]
    song.sample(8, sweep, volume=60, loop=(2048, 2048), name="long sweep")
    for cell in cells:
        song.note(*cell)
    song.positions = list(positions)
    return song.build()


def _rows(pattern, channel, start, step, effects, period=0, sample=0):
    """One cell every `step` rows from `start`, cycling through (effect, param)."""
    return [(pattern, start + i * step, channel, period, sample, e, p)
            for i, (e, p) in enumerate(effects)]


def volume_set_song():
    cells = [(0, 0, 0, C2, 1, 0, 0), (0, 0, 1, E2, 2, 0, 0), (0, 0, 2, G2, 3, 0xC, 20)]
    cells += _rows(0, 0, 4, 4, [(0xC, v) for v in (0, 10, 32, 64, 65, 0x7F, 0x80, 0xFF, 40)])
    cells += _rows(0, 1, 2, 6, [(0xC, v) for v in (64, 3, 0x40, 0x41, 0)])
    cells += [(0, 40, 2, C2, 3, 0xC, 50), (0, 48, 2, C2, 3, 0, 0)]  # Sample volume returns
    return effect_song("volume set", cells)


def volume_slide_song():
    cells = [(0, 0, 0, C2, 1, 0xC, 10), (0, 0, 1, E2, 2, 0xC, 60), (0, 0, 3, G2, 1, 0, 0)]
    cells += _rows(0, 0, 1, 1, [(0xA, 0x40)] * 12 + [(0xA, 0x0F)] * 8)   # up past 64, down past 0
    cells += _rows(0, 1, 1, 1, [(0xA, 0x03)] * 25 + [(0xA, 0x21)] * 6)   # both nibbles: up wins
    cells += _rows(0, 3, 4, 2, [(0xA, 0x00), (0xA, 0x10), (0xA, 0x01), (0xA, 0xF0)])
    return effect_song("volume slide", cells)


def tremolo_song():
    cells = [(0, 0, 0, C2, 1, 7, 0x44), (0, 0, 1, E2, 2, 0, 0), (0, 0, 2, G2, 1, 0, 0)]
    cells += _rows(0, 0, 1, 1, [(7, 0x00)] * 6 + [(7, 0x80)] * 4 + [(7, 0x0F)] * 4)  # memory
    cells += _rows(0, 1, 1, 1, [(0xE, 0x71)] + [(7, 0x68)] * 10)   # ramp waveform
    cells += _rows(0, 1, 12, 1, [(0xE, 0x72)] + [(7, 0x3C)] * 8)   # square
    cells += _rows(0, 1, 21, 1, [(0xE, 0x73)] + [(7, 0x5A)] * 8)   # random = square in PT
    cells += _rows(0, 1, 30, 1, [(0xE, 0x74)] + [(7, 0x8F)] * 4)   # no retrigger reset
    cells += [(0, 35, 1, E2, 2, 7, 0x00), (0, 36, 1, E2, 2, 0xE, 0x70), (0, 37, 1, E2, 2, 7, 0)]
    return effect_song("tremolo", cells)


def volume_fine_song():
    cells = [(0, 0, 0, C2, 1, 0xC, 30), (0, 0, 1, E2, 2, 0xC, 2)]
    cells += _rows(0, 0, 1, 1, [(0xE, 0xA4)] * 10 + [(0xE, 0xBF)] * 6)
    cells += _rows(0, 1, 1, 2, [(0xE, 0xB1), (0xE, 0xB5), (0xE, 0xA0), (0xE, 0xAF)] * 3)
    return effect_song("volume fine", cells)


def arpeggio_song():
    cells = []
    for ch, (period, sample) in enumerate(((C2, 1), (C3, 4), (B1, 6), (A2, 5))):
        cells.append((0, 0, ch, period, sample, 0, 0x37))
        cells += _rows(0, ch, 1, 1, [(0, 0x47), (0, 0xC0), (0, 0x0C), (0, 0xFF), (0, 0)])
    # high notes: base + offset runs off the end of the finetune section
    cells += [(0, 8, 0, 113, 1, 0, 0xFF), (0, 9, 1, 113, 4, 0, 0xFF),
              (0, 10, 2, 113, 6, 0, 0xFF), (0, 11, 3, 113, 7, 0, 0xFF)]
    # arpeggio on a period that is not in the table (after a slide)
    cells += [(0, 16, 0, C2, 1, 1, 3), (0, 17, 0, 0, 0, 0, 0x47), (0, 18, 0, 0, 0, 0, 0x47)]
    cells += [(0, 20, 1, 907, 6, 0, 0x3F)]
    return effect_song("arpeggio", cells)


def portamento_song():
    cells = [(0, 0, 0, C2, 1, 1, 2), (0, 0, 1, C2, 2, 2, 3), (0, 0, 2, C3, 4, 1, 0x40),
             (0, 0, 3, C1, 5, 2, 0x40)]
    cells += _rows(0, 0, 1, 1, [(1, 2)] * 6 + [(1, 0)] * 2 + [(2, 0x10)] * 4)
    cells += _rows(0, 1, 1, 1, [(2, 3)] * 3 + [(0xE, 0x2F), (0xE, 0x1F), (0xE, 0x15)] * 3)
    cells += _rows(0, 2, 1, 1, [(1, 0x40)] * 4)     # clamps at 113
    cells += _rows(0, 3, 1, 1, [(2, 0x40)] * 4)     # clamps at 856
    # ProTracker bug: the 113 check ignores the sign, so a big slide wraps
    cells += [(0, 12, 2, 120, 1, 1, 0xFF), (0, 13, 2, 0, 0, 1, 0xFF), (0, 14, 2, 0, 0, 0, 0x37),
              (0, 15, 2, 0, 0, 4, 0x44), (0, 16, 2, 0, 0, 3, 0x10)]
    cells += _rows(0, 0, 20, 1, [(0xE, 0x11), (0xE, 0x10), (0xE, 0x21), (0xE, 0x2F)])
    return effect_song("portamento", cells)


def tone_portamento_song():
    cells = [(0, 0, 0, C2, 1, 0, 0), (0, 2, 0, C3, 0, 3, 4)]
    cells += _rows(0, 0, 3, 1, [(3, 0)] * 6)                  # memory
    cells += [(0, 9, 0, C1, 0, 3, 0x20), (0, 10, 0, 0, 0, 5, 0x02), (0, 11, 0, 0, 0, 5, 0x20),
              (0, 12, 0, C1, 0, 3, 0)]                         # same period: nothing to do
    cells += [(0, 0, 1, C2, 4, 0, 0), (0, 1, 1, E2, 4, 3, 1), (0, 2, 1, G2, 6, 3, 2),
              (0, 3, 1, 0, 0, 0xE, 0x31), (0, 4, 1, C3, 6, 3, 3), (0, 5, 1, 0, 0, 3, 0),
              (0, 6, 1, 0, 0, 3, 0), (0, 7, 1, 0, 0, 0xE, 0x30), (0, 8, 1, C1, 5, 3, 8)]
    cells += [(0, 0, 2, 0, 0, 3, 5), (0, 1, 2, C2, 2, 3, 0x10),   # tone porta from silence
              (0, 4, 2, 4000, 2, 3, 0x10), (0, 6, 2, 50, 2, 3, 0x10)]
    cells += [(0, 16, 3, C2, 7, 0, 0), (0, 17, 3, A2, 7, 5, 0x41), (0, 18, 3, 0, 0, 5, 0x0F)]
    # fast slides that reach their target, so the target period itself is
    # checked (negative finetunes pick the entry one above: a PT quirk)
    for i, (sample, a, b) in enumerate(((6, C2, G2), (4, E2, C3), (6, C1, C3), (5, G2, C2),
                                        (4, 856, 113), (6, 113, 856))):
        cells += [(0, 24 + i * 3, 2, a, sample, 0, 0), (0, 25 + i * 3, 2, b, sample, 3, 0xFF)]
    return effect_song("tone portamento", cells)


def vibrato_song():
    cells = [(0, 0, 0, C2, 1, 4, 0x48)]
    cells += _rows(0, 0, 1, 1, [(4, 0), (4, 0x80), (4, 0x0F), (4, 0), (6, 0x01), (6, 0x10)])
    cells += [(0, 0, 1, E2, 2, 0xE, 0x41)] + _rows(0, 1, 1, 1, [(4, 0x6A)] * 6)   # ramp
    cells += [(0, 8, 1, E2, 2, 0xE, 0x42)] + _rows(0, 1, 9, 1, [(4, 0x5C)] * 4)   # square
    cells += [(0, 14, 1, E2, 2, 0xE, 0x43)] + _rows(0, 1, 15, 1, [(4, 0x5C)] * 4)  # random
    cells += [(0, 20, 1, E2, 2, 0xE, 0x44), (0, 21, 1, E2, 2, 4, 0x44), (0, 22, 1, G2, 2, 4, 0),
              (0, 23, 1, C2, 2, 0xE, 0x40), (0, 24, 1, C2, 2, 4, 0x44)]  # no-retrigger flag
    cells += [(0, 0, 2, G2, 4, 4, 0xFF), (0, 1, 2, 0, 0, 4, 0)]
    # ramp tremolo reads the vibrato position (PT bug)
    cells += [(0, 0, 3, C2, 1, 0, 0)] + _rows(0, 3, 1, 1, [(4, 0x88)] * 10 + [(0xE, 0x71)] +
                                              [(7, 0x48)] * 10)
    return effect_song("vibrato", cells)


def finetune_song():
    cells = []
    for i, period in enumerate((C1, E1, G1, C2, E2, G2, C3, E3, G3, 113, 856, 900, 100)):
        for ch, sample in enumerate((4, 5, 6, 7)):
            cells.append((0, i * 2, ch, period, sample, 0, 0))
    return effect_song("finetune", cells)


def _marker(pattern, row, period):
    """A note on channel 4 so each Pattern/Row is recognisable in the log."""
    return (pattern, row, 3, period, 1, 0, 0)


def position_jump_song():
    cells = [_marker(p, 0, per) for p, per in ((0, C1), (1, D1), (2, E1), (3, F1), (4, G1))]
    cells += [(0, 4, 0, C2, 2, 0xB, 3),                       # jump to Position 3
              (3, 2, 0, 0, 0, 0xD, 0x12),                     # break to row 12 (decimal)
              (1, 10, 0, 0, 0, 0xD, 0x70),                    # break past 63 -> row 0
              (2, 20, 0, 0, 0, 0xB, 1), (2, 20, 1, 0, 0, 0xD, 0x05),  # B then D, same row
              (4, 5, 1, 0, 0, 0xD, 0x30), (4, 5, 0, 0, 0, 0xB, 2),    # D then B
              (4, 63, 0, 0, 0, 0xD, 0x63),                    # break on the last row
              (1, 30, 2, 0, 0, 0xB, 0x7F)]                    # past the end -> Position 0
    return effect_song("position jump", cells, positions=(0, 3, 1, 4, 2, 1))


def pattern_loop_song():
    cells = [_marker(0, r, C2 + r) for r in range(0, 64, 4)]
    cells += [(0, 2, 0, 0, 0, 0xE, 0x60), (0, 5, 0, 0, 0, 0xE, 0x62),      # loop rows 2-5 x2
              (0, 8, 1, 0, 0, 0xE, 0x60), (0, 9, 2, 0, 0, 0xE, 0x60),      # nested, two channels
              (0, 11, 2, 0, 0, 0xE, 0x61), (0, 13, 1, 0, 0, 0xE, 0x62),
              (0, 16, 0, 0, 0, 0xE, 0x63),                                 # loop on one row
              (0, 16, 1, 0, 0, 0xE, 0x60),
              (0, 20, 0, 0, 0, 0xE, 0x61),                                 # back to last E60
              (0, 30, 2, 0, 0, 0xE, 0x60), (0, 31, 2, 0, 0, 0xE, 0x61),
              (0, 31, 1, 0, 0, 0xD, 0x40)]                                 # loop + break
    cells += [(1, 3, 0, 0, 0, 0xE, 0x62), (1, 6, 1, 0, 0, 0xE, 0x60),      # no E60 yet
              (1, 7, 1, 0, 0, 0xE, 0x61)]
    return effect_song("pattern loop", cells, positions=(0, 1))


def pattern_delay_song():
    cells = [(0, 0, 0, C2, 1, 0, 0), (0, 0, 1, E2, 2, 0xC, 20)]
    cells += [(0, 2, 0, 0, 0, 0xE, 0xE2), (0, 2, 1, 0, 0, 0xE, 0xA4),      # fine slide repeats
              (0, 4, 0, G2, 3, 0xE, 0xE1), (0, 4, 1, 0, 0, 0xA, 0x02),     # note + slide in delay
              (0, 6, 0, 0, 0, 0xE, 0xE3), (0, 6, 2, 0, 0, 0xE, 0xE1),      # two delays, same row
              (0, 8, 0, 0, 0, 0xE, 0x60), (0, 9, 0, 0, 0, 0xE, 0x61),
              (0, 9, 1, 0, 0, 0xE, 0xE2),                                  # delay on a loop row
              (0, 12, 0, 0, 0, 0xE, 0xEF), (0, 12, 1, C2, 2, 0xD, 0x00),   # delay + break
              (0, 13, 0, 0, 0, 0xE, 0xE0)]
    cells += [(1, 1, 0, C3, 1, 0xE, 0xE1), (1, 1, 1, 0, 0, 0xE, 0x91), (1, 1, 2, C2, 2, 0xE, 0xD2)]
    return effect_song("pattern delay", cells, positions=(0, 1))


def tempo_song():
    cells = [(0, r, 0, C2, 1, 0, 0) for r in range(0, 64, 2)]
    cells += [(0, 2, 1, 0, 0, 0xF, 3), (0, 4, 1, 0, 0, 0xF, 0x20),        # speed, then CIA 32 BPM
              (0, 5, 1, 0, 0, 0xF, 0xFF), (0, 6, 1, 0, 0, 0xF, 0x7D),
              (0, 8, 1, 0, 0, 0xF, 1), (0, 8, 2, 0, 0, 0xF, 150),          # speed + tempo, one row
              (0, 12, 1, 0, 0, 0xF, 31), (0, 13, 1, 0, 0, 0xF, 6),
              (0, 14, 1, 0, 0, 0xF, 0x40), (0, 14, 2, 0, 0, 0xF, 0x50),    # two tempos: last wins
              (0, 20, 3, 0, 0, 0xF, 0x00)]                                  # stop
    return effect_song("tempo", cells)


def sample_offset_song():
    S = 8
    cells = [(0, 0, 0, C2, S, 9, 4), (0, 2, 0, C2, S, 9, 0),        # offset, then memory
             (0, 4, 0, C2, S, 9, 0x20), (0, 6, 0, C2, S, 9, 0xFF),  # past the end: length 1
             (0, 8, 0, 0, 0, 9, 2), (0, 10, 0, C2, 0, 0, 0),        # no note: moves the start
             (0, 12, 0, C2, S, 9, 0x08), (0, 13, 0, E2, 0, 3, 0x40),
             (0, 14, 0, 0, 0, 9, 1), (0, 16, 0, G2, S, 0xE, 0x91)]  # retrigger after offset
    cells += [(0, 0, 1, C2, 3, 9, 1), (0, 4, 1, C2, 1, 9, 1), (0, 8, 1, C2, 3, 9, 3),
              (0, 12, 1, E2, 2, 9, 0)]
    return effect_song("sample offset", cells)


def retrigger_song():
    cells = [(0, 0, 0, C2, 3, 0xE, 0x91), (0, 2, 0, 0, 0, 0xE, 0x92), (0, 4, 0, E2, 0, 0xE, 0x93),
             (0, 6, 0, 0, 0, 0xE, 0x90), (0, 8, 0, C2, 3, 0xE, 0x95), (0, 10, 0, 0, 0, 0xE, 0x9F),
             (0, 12, 0, 0, 0, 0xF, 12), (0, 13, 0, 0, 0, 0xE, 0x94), (0, 14, 0, G2, 1, 0xE, 0x97)]
    cells += [(0, 1, 1, 0, 0, 0xE, 0x92), (0, 3, 1, E2, 2, 0xE, 0x91)]   # nothing played yet
    return effect_song("retrigger", cells)


def note_cut_delay_song():
    cells = [(0, 0, 0, C2, 1, 0xE, 0xC2), (0, 2, 0, C2, 1, 0xE, 0xC0), (0, 4, 0, C2, 1, 0xE, 0xC9),
             (0, 6, 0, E2, 2, 0xE, 0xC5), (0, 7, 0, 0, 0, 0xE, 0xC1)]
    cells += [(0, 0, 1, C2, 3, 0xE, 0xD2), (0, 2, 1, E2, 3, 0xE, 0xD0), (0, 4, 1, G2, 1, 0xE, 0xD7),
              (0, 6, 1, 0, 0, 0xE, 0xD3), (0, 8, 1, C3, 2, 0xE, 0xD5), (0, 9, 1, 0, 4, 0xE, 0xD1),
              (0, 10, 1, C2, 0, 0xE, 0xD4)]
    return effect_song("note cut and delay", cells)


def finetune_effect_song():
    cells = [(0, 0, 0, C2, 1, 0xE, 0x57), (0, 1, 0, C2, 1, 0, 0), (0, 2, 0, 0, 0, 0xE, 0x5F),
             (0, 3, 0, E2, 0, 0, 0), (0, 4, 0, E2, 0, 0, 0x37), (0, 6, 0, G2, 4, 0xE, 0x50),
             (0, 8, 0, C2, 0, 3, 8), (0, 10, 0, C3, 0, 0xE, 0x58), (0, 11, 0, C3, 0, 3, 0x40)]
    return effect_song("finetune effect", cells)


def invert_loop_song():
    cells = [(0, 0, 0, C2, 1, 0xE, 0xF5), (0, 8, 0, 0, 0, 0xE, 0xFF), (0, 12, 0, 0, 0, 0xE, 0xF0),
             (0, 14, 0, E2, 2, 0xE, 0xF8), (0, 20, 0, C2, 3, 0xE, 0xFD),       # non-looping
             (0, 24, 0, C2, 8, 0xE, 0xFC), (0, 30, 0, 0, 1, 0xE, 0xF1)]        # Sample change
    cells += [(0, 2, 1, E2, 1, 0xE, 0xF3), (0, 4, 1, 0, 0, 1, 1), (0, 5, 1, 0, 0, 0xA, 1)]
    return effect_song("invert loop", cells, positions=(0, 0))


def filter_song():
    cells = [(0, 0, 0, C2, 1, 0xE, 0x00), (0, 2, 0, 0, 0, 0xE, 0x01), (0, 4, 1, 0, 0, 0xE, 0x02),
             (0, 6, 2, 0, 0, 0xE, 0x03), (0, 8, 3, 0, 0, 0xE, 0x0E), (0, 9, 0, 0, 0, 0xE, 0x80)]
    return effect_song("filter", cells)


GENERATED = {
    "plain-notes.mod": plain_notes_song,
    "volume-set.mod": volume_set_song,
    "volume-slide.mod": volume_slide_song,
    "tremolo.mod": tremolo_song,
    "volume-fine.mod": volume_fine_song,
    "arpeggio.mod": arpeggio_song,
    "portamento.mod": portamento_song,
    "tone-portamento.mod": tone_portamento_song,
    "vibrato.mod": vibrato_song,
    "finetune.mod": finetune_song,
    "position-jump.mod": position_jump_song,
    "pattern-loop.mod": pattern_loop_song,
    "pattern-delay.mod": pattern_delay_song,
    "tempo.mod": tempo_song,
    "sample-offset.mod": sample_offset_song,
    "retrigger.mod": retrigger_song,
    "note-cut-delay.mod": note_cut_delay_song,
    "finetune-effect.mod": finetune_effect_song,
    "invert-loop.mod": invert_loop_song,
    "filter.mod": filter_song,
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
