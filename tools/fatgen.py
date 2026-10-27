#!/usr/bin/env python3
"""Makes FAT32 card images for testing the firmware's FAT32 reader (issue
#26), as Microsoft's FAT specification lays them out: 512-byte sectors, two
FATs, an FSInfo sector and a backup boot sector, the root directory a
cluster chain, optionally behind an MBR partition table.

    img = Fat32Image(mbr=True)
    img.add_file("My Song.mod", data)               # long name, with a short alias
    img.add_file("SHORT.MOD", data, fragment=True)  # clusters scattered
    open("card.img", "wb").write(img.build())

Names that are valid 8.3 names in upper case get just a short entry; any
other gets long-name entries before its short alias (NAME~1.EXT). Images
are small (a few MB) with one sector a cluster, so files and the root
directory span many clusters; the FAT32 specification's minimum of 65 525
clusters is not kept, so the reader must not depend on it.

Ways to damage an image, for the error tests: break_chain() (a free, bad or
out-of-range FAT entry in a file's chain), and add_file(size=...) to give a
directory entry a different size from its data. Oddities a reader must
cope with: Fat32Image(high_bits=True) sets the top 4 bits of every used FAT
entry, which the specification reserves and readers must ignore;
add_file(orphan_lfn=True) gives a long name a checksum that doesn't match
its 8.3 entry (as tools that know only 8.3 names leave behind on renaming),
so only the 8.3 name is the file's.
"""
import struct
import sys

SECTOR = 512
EOC = 0x0FFFFFFF


def short_ok(name):
    """Is name already a valid upper-case 8.3 name?"""
    base, dot, ext = name.partition(".")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^`{}")
    return (0 < len(base) <= 8 and len(ext) <= 3 and "." not in ext and
            all(c in ok for c in base + ext))


def short_entry_name(name):
    base, _, ext = name.partition(".")
    return base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def lfn_checksum(short11):
    s = 0
    for b in short11:
        s = (((s & 1) << 7) | (s >> 1)) + b & 0xFF
    return s


class Entry:
    def __init__(self, name, data, attr, size=None, deleted=False):
        self.name, self.data, self.attr, self.deleted = name, data, attr, deleted
        self.orphan_lfn = False
        self.size = len(data) if size is None else size
        self.clusters = []
        self.children = []                      # for a directory


class Fat32Image:
    def __init__(self, total_sectors=8192, mbr=True, part_start=2048, sectors_per_cluster=1,
                 reserved=32, fats=2, label="MODPLAYER", high_bits=False):
        self.high_bits = high_bits
        self.mbr, self.part_start = mbr, part_start if mbr else 0
        self.vol_sectors = total_sectors - self.part_start
        self.spc, self.reserved, self.fats, self.label = sectors_per_cluster, reserved, fats, label
        # sectors per FAT: enough for every cluster the rest of the volume holds
        spf = 1
        while True:
            clusters = (self.vol_sectors - reserved - fats * spf) // self.spc
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= spf:
                break
            spf = need
        self.spf, self.clusters = spf, clusters
        self.fat = [0] * (clusters + 2)
        self.fat[0], self.fat[1] = 0x0FFFFFF8, EOC
        self.next_free = 3                       # cluster 2 is the root directory's first
        self.root = Entry("", b"", 0x10)
        self.root.clusters = [2]
        self.fat[2] = EOC
        self.files = []
        self.short_names = set()

    @property
    def cluster_bytes(self):
        return self.spc * SECTOR

    def allocate(self, n, fragment=False):
        """n free clusters; with fragment, every other free one (leaving gaps for later files)."""
        out = []
        c = self.next_free
        while len(out) < n:
            if c >= len(self.fat):
                raise ValueError("image full")
            if self.fat[c] == 0:
                out.append(c)
                self.fat[c] = EOC                # taken (linked below)
                c += 2 if fragment else 1
            else:
                c += 1
        if not fragment:
            self.next_free = c
        else:
            while self.next_free < len(self.fat) and self.fat[self.next_free] != 0:
                self.next_free += 1
        return out

    def link(self, clusters):
        for a, b in zip(clusters, clusters[1:]):
            self.fat[a] = b
        if clusters:
            self.fat[clusters[-1]] = EOC

    def short_alias(self, name):
        base, _, ext = name.upper().partition(".")
        keep = "".join(c for c in base if c.isalnum()) or "FILE"
        ext = "".join(c for c in ext if c.isalnum())[:3]
        for n in range(1, 10000):
            tail = "~%d" % n
            alias = keep[:8 - len(tail)] + tail + ("." + ext if ext else "")
            if alias not in self.short_names:
                self.short_names.add(alias)
                return alias
        raise ValueError("too many aliases")

    def name_entry(self, e):
        """Gives the entry its 8.3 name: its own, or an alias for a long name."""
        if short_ok(e.name):
            e.short, e.long = e.name, False
        else:
            e.short, e.long = self.short_alias(e.name), True
        self.short_names.add(e.short)

    def add_file(self, name, data, fragment=False, size=None, deleted=False, directory=None, at_cluster=None,
                 orphan_lfn=False):
        """at_cluster: put the file's clusters from there on (to test cluster numbers over 65535)."""
        e = Entry(name, data, 0x20, size=size, deleted=deleted)
        e.orphan_lfn = orphan_lfn
        self.name_entry(e)
        if at_cluster is not None:
            saved, self.next_free = self.next_free, at_cluster
        n = max(1, (len(data) + self.cluster_bytes - 1) // self.cluster_bytes) if data else 0
        e.clusters = self.allocate(n, fragment)
        if at_cluster is not None:
            self.next_free = saved
        self.link(e.clusters)
        (directory or self.root).children.append(e)
        self.files.append(e)
        return e

    def add_dir(self, name):
        d = Entry(name, b"", 0x10)
        self.name_entry(d)
        d.clusters = self.allocate(1)
        self.link(d.clusters)
        self.root.children.append(d)
        return d

    def break_chain(self, entry, index, value):
        """Sets the FAT entry of the file's index-th cluster to value (0 free, 0x0FFFFFF7 bad, ...)."""
        self.fat[entry.clusters[index]] = value

    # ---- building ---------------------------------------------------------------
    def dir_entries(self, d):
        out = bytearray()
        if d is self.root:
            vol = bytearray(32)
            vol[0:11] = self.label.ljust(11).encode("ascii")[:11]
            vol[11] = 0x08
            out += vol
        for e in d.children:
            short = e.short
            lfn = list(e.name) if e.long else []
            s11 = short_entry_name(short)
            assert len(s11) == 11, short
            if lfn:
                chars = [ord(c) for c in lfn] + [0]
                while len(chars) % 13:
                    chars.append(0xFFFF)
                parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
                for seq in range(len(parts), 0, -1):
                    p = parts[seq - 1]
                    ent = bytearray(32)
                    ent[0] = seq | (0x40 if seq == len(parts) else 0)
                    ent[1:11] = struct.pack("<5H", *p[0:5])
                    ent[11] = 0x0F
                    ent[13] = lfn_checksum(s11) ^ (0x5A if e.orphan_lfn else 0)
                    ent[14:26] = struct.pack("<6H", *p[5:11])
                    ent[28:32] = struct.pack("<2H", *p[11:13])
                    if e.deleted:
                        ent[0] = 0xE5
                    out += ent
            ent = bytearray(32)
            ent[0:11] = s11
            ent[11] = e.attr
            first = e.clusters[0] if e.clusters else 0
            struct.pack_into("<HH", ent, 20, first >> 16, 0)
            struct.pack_into("<HI", ent, 26, first & 0xFFFF, e.size)
            if e.deleted:
                ent[0] = 0xE5
            out += ent
        return out

    def build(self):
        # directories first: their size decides how many clusters they need
        for d in [self.root] + [e for e in self.root.children if e.attr & 0x10]:
            d.data = bytes(self.dir_entries(d))
            need = max(1, (len(d.data) + self.cluster_bytes - 1) // self.cluster_bytes)
            if need > len(d.clusters):
                more = self.allocate(need - len(d.clusters), fragment=True)
                d.clusters += more
                self.link(d.clusters)
        img = bytearray(SECTOR * (self.part_start + self.vol_sectors))
        v = self.part_start * SECTOR
        if self.mbr:
            p = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0x0C, b"\xfe\xff\xff", self.part_start, self.vol_sectors)
            img[446:462] = p
            img[510:512] = b"\x55\xaa"
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xeb\x58\x90"
        bs[3:11] = b"MSWIN4.1"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, self.spc, self.reserved, self.fats, 0, 0, 0xF8, 0,
                         63, 255, self.part_start, self.vol_sectors)
        struct.pack_into("<IHHIHH", bs, 36, self.spf, 0, 0, 2, 1, 6)
        bs[64], bs[66] = 0x80, 0x29
        struct.pack_into("<I", bs, 67, 0x12345678)
        bs[71:82] = self.label.ljust(11).encode("ascii")[:11]
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xaa"
        img[v:v + SECTOR] = bs
        img[v + 6 * SECTOR:v + 7 * SECTOR] = bs                    # backup boot sector
        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<III", fsinfo, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF)
        fsinfo[510:512] = b"\x55\xaa"
        img[v + SECTOR:v + 2 * SECTOR] = fsinfo
        entries = [v | 0xF0000000 if self.high_bits and v and i >= 2 else v for i, v in enumerate(self.fat)]
        fat = struct.pack("<%dI" % len(entries), *entries)
        for k in range(self.fats):
            at = v + (self.reserved + k * self.spf) * SECTOR
            img[at:at + len(fat)] = fat
        data_start = v + (self.reserved + self.fats * self.spf) * SECTOR
        for e in [self.root] + self.files + [c for c in self.root.children if c.attr & 0x10]:
            for i, c in enumerate(e.clusters):
                chunk = e.data[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
                at = data_start + (c - 2) * self.cluster_bytes
                img[at:at + len(chunk)] = chunk
        return bytes(img)


if __name__ == "__main__":
    # a card image from files: fatgen.py out.img file...
    img = Fat32Image(total_sectors=65536)
    for path in sys.argv[2:]:
        img.add_file(path.replace("\\", "/").split("/")[-1], open(path, "rb").read())
    open(sys.argv[1], "wb").write(img.build())
