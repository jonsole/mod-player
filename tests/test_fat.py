"""Songs from a FAT32 card (issue #26): the simulated chip, driven over its
serial port, reading card images made by tools/fatgen.py through the SD card
model (hw/sim/spi_sd_model.h)."""
import os
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fatgen  # noqa: E402
import modgen  # noqa: E402
import modplayer  # noqa: E402

SOC_TB = os.path.join(ROOT, "build", "soc_tb")


def song(name="card song", notes=4):
    s = modgen.Song(name)
    s.sample(1, modgen.sine_cycle(32) * 8, volume=50, loop=(0, 256), name="sine")
    for row in range(notes):
        s.note(0, row * 2, row % 4, period=modgen.C2 + 10 * row, sample=1)
    return s.build()


def short_song(name):
    """A Song that ends almost at once: speed 1, and a pattern break on its first row."""
    s = modgen.Song(name)
    s.sample(1, modgen.sine_cycle(32) * 8, volume=50, loop=(0, 256), name="sine")
    s.note(0, 0, 0, period=modgen.C2, sample=1, effect=0xF, param=1)
    s.note(0, 0, 1, effect=0xD, param=0)
    return s.build()


def data(n, seed=1):
    """n bytes that differ from block to block."""
    return bytes((i * 31 + (i >> 9) * 7 + seed) & 0xFF for i in range(n))


def run_card(image, expect, send=b"", after="ready", switches=0, max_cycles=400000000):
    tmp = tempfile.mkdtemp()
    path = os.path.join(tmp, "card.img")
    with open(path, "wb") as f:
        f.write(image)
    args = [SOC_TB, "--max", str(max_cycles), "--sd-image", path, "--switches", str(switches)]
    if send:
        sp = os.path.join(tmp, "send.bin")
        with open(sp, "wb") as f:
            f.write(send)
        args += ["--after", after, "--send", sp]
    args.append(expect)
    result = subprocess.run(args, capture_output=True, text=True)
    return result.returncode, result.stdout


def crc_reply(content):
    return "OK crc %08x size %d" % (zlib.crc32(content), len(content))


class Fat32(unittest.TestCase):
    def test_a_short_named_file_reads_back(self):
        img = fatgen.Fat32Image()
        content = data(5000)
        img.add_file("SHORT.MOD", content)
        rc, out = run_card(img.build(), crc_reply(content), modplayer.encode_card_crc("SHORT.MOD"))
        self.assertEqual(rc, 0, out)

    def test_names_match_ignoring_case_long_or_short(self):
        img = fatgen.Fat32Image()
        content = data(3000, seed=5)
        img.add_file("Space Debris (remix).mod", content)
        for name in ("space debris (REMIX).MOD", "SPACED~1.MOD"):
            rc, out = run_card(img.build(), crc_reply(content), modplayer.encode_card_crc(name))
            self.assertEqual(rc, 0, name + ": " + out)

    def test_a_long_name_over_several_entries_and_a_fragmented_file(self):
        img = fatgen.Fat32Image()
        filler = img.add_file("FILLER.BIN", data(512 * 3))
        name = "a very long name that needs three long-name entries.mod"
        content = data(512 * 70 + 123, seed=9)               # 71 clusters, every other one
        img.add_file(name, content, fragment=True)
        img.add_file("AFTER.BIN", data(512 * 40, seed=3))    # fills the gaps
        rc, out = run_card(img.build(), crc_reply(content), modplayer.encode_card_crc(name))
        self.assertEqual(rc, 0, out)
        self.assertTrue(filler.clusters)

    def test_cluster_numbers_over_65535(self):
        # the first cluster's high 16 bits are in another field of the entry
        img = fatgen.Fat32Image(total_sectors=2048 + 72000)
        content = data(3000, seed=7)
        e = img.add_file("HIGH.MOD", content, at_cluster=70000)
        self.assertGreater(e.clusters[0], 65535)
        rc, out = run_card(img.build(), crc_reply(content), modplayer.encode_card_crc("HIGH.MOD"))
        self.assertEqual(rc, 0, out)

    def test_the_reserved_top_bits_of_fat_entries_are_ignored(self):
        img = fatgen.Fat32Image(high_bits=True)
        content = data(512 * 12 + 7, seed=8)
        img.add_file("TOPBITS.MOD", content, fragment=True)
        rc, out = run_card(img.build(), crc_reply(content), modplayer.encode_card_crc("TOPBITS.MOD"))
        self.assertEqual(rc, 0, out)

    def test_a_long_name_whose_checksum_doesnt_match_is_not_the_files(self):
        img = fatgen.Fat32Image()
        content = data(800, seed=11)
        e = img.add_file("Renamed long name.mod", content, orphan_lfn=True)
        built = img.build()
        rc, out = run_card(built, "ERR file not found", modplayer.encode_card_crc("renamed long name.mod"))
        self.assertEqual(rc, 0, out)
        rc, out = run_card(built, crc_reply(content), modplayer.encode_card_crc(e.short))
        self.assertEqual(rc, 0, out)

    def test_a_card_without_a_partition_table(self):
        img = fatgen.Fat32Image(mbr=False)
        content = data(2000, seed=2)
        img.add_file("NOMBR.MOD", content)
        rc, out = run_card(img.build(), crc_reply(content), modplayer.encode_card_crc("nombr.mod"))
        self.assertEqual(rc, 0, out)

    def test_a_root_directory_of_many_clusters(self):
        img = fatgen.Fat32Image()
        for i in range(60):                                   # 16 entries a cluster: several clusters
            img.add_file("Song number %02d.mod" % i, data(100, seed=i))
        content = data(700, seed=60)
        img.add_file("LAST.MOD", content)
        rc, out = run_card(img.build(), crc_reply(content), modplayer.encode_card_crc("LAST.MOD"))
        self.assertEqual(rc, 0, out)

    def test_deleted_files_and_subdirectories_are_not_found(self):
        img = fatgen.Fat32Image()
        img.add_file("GONE.MOD", data(600), deleted=True)
        sub = img.add_dir("SONGS")
        img.add_file("INSUB.MOD", data(600), directory=sub)
        content = data(900, seed=4)
        img.add_file("gone.mod but live.mod", content)
        built = img.build()
        for name in ("GONE.MOD", "INSUB.MOD", "SONGS", "missing.mod"):
            rc, out = run_card(built, "ERR file not found", modplayer.encode_card_crc(name))
            self.assertEqual(rc, 0, name + ": " + out)

    def test_a_damaged_chain_is_an_error(self):
        for value in (0, 0x0FFFFFF7, 0x0FFFFFFF, 0x0123456):     # free, bad, ended early, out of range
            img = fatgen.Fat32Image()
            e = img.add_file("BROKEN.MOD", data(512 * 10))
            img.break_chain(e, 4, value)
            rc, out = run_card(img.build(), "ERR damaged FAT chain", modplayer.encode_card_crc("BROKEN.MOD"))
            self.assertEqual(rc, 0, "%x: %s" % (value, out))

    def test_a_card_without_fat32_is_an_error(self):
        image = bytearray(fatgen.Fat32Image().build())
        image[2048 * 512 + 17] = 0x02                         # root entries: FAT12/16, not FAT32
        rc, out = run_card(bytes(image), "ERR card not FAT32", modplayer.encode_card_crc("X.MOD"))
        self.assertEqual(rc, 0, out)
        rc, out = run_card(bytes(512 * 64), "ERR card not FAT32", modplayer.encode_card_crc("X.MOD"))
        self.assertEqual(rc, 0, out)

    def test_a_file_larger_than_the_song_memory_is_refused(self):
        img = fatgen.Fat32Image()
        img.add_file("HUGE.MOD", data(512), size=0x7FFFFFFF)
        rc, out = run_card(img.build(), "ERR file too large", modplayer.encode_card_crc("HUGE.MOD"))
        self.assertEqual(rc, 0, out)

    def test_the_root_directory_lists_its_files(self):
        img = fatgen.Fat32Image()
        img.add_file("Xenon II.mod", data(1234))
        img.add_file("GODS.MOD", data(99, seed=2))
        img.add_file("OLD.MOD", data(10), deleted=True)
        img.add_dir("SONGS")
        rc, out = run_card(img.build(), "OK 2 files", modplayer.encode_simple("card-ls"))
        self.assertEqual(rc, 0, out)
        self.assertIn("file 1234 Xenon II.mod", out)
        self.assertIn("file 99 GODS.MOD", out)
        self.assertNotIn("OLD", out)

    def test_a_song_from_the_card_plays(self):
        img = fatgen.Fat32Image()
        img.add_file("My Song.mod", song("from the card"))
        rc, out = run_card(img.build(), "OK from the card", modplayer.encode_card_play("my song.mod"))
        self.assertEqual(rc, 0, out)

    def test_at_power_on_the_cards_default_song_plays(self):
        img = fatgen.Fat32Image()
        img.add_file("other.mod", song("not this one"))
        img.add_file("Default.MOD", song("the default"))
        rc, out = run_card(img.build(), "card Song: the default")
        self.assertEqual(rc, 0, out)

    def test_developer_mode_leaves_the_default_song_alone(self):
        img = fatgen.Fat32Image()
        img.add_file("default.mod", song("the default"))
        rc, out = run_card(img.build(), "OK mod-player", modplayer.encode_simple("version"), switches=8)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("card Song", out)

    def test_with_sw3_on_the_next_song_on_the_card_follows_wrapping_round(self):
        img = fatgen.Fat32Image()
        img.add_file("first.mod", short_song("song one"))
        img.add_file("README.TXT", b"not a Song")                 # not a Song's name: passed over
        img.add_file("MOD.SECOND", short_song("song two"))        # the Amiga's naming
        img.add_file("broken.mod", b"not a MOD file at all" * 100) # won't load: passed over
        img.add_file("Third One.mod", short_song("song three"))
        rc, out = run_card(img.build(), "next Song: song one", modplayer.encode_card_play("FIRST.MOD"),
                           switches=4)
        self.assertEqual(rc, 0, out)
        at = [out.index(t) for t in ("OK song one", "next Song: song two", "next Song: song three",
                                     "next Song: song one")]
        self.assertEqual(at, sorted(at), out)
        self.assertNotIn("README", out)                       # not even tried
        self.assertIn("card Song not playable: broken.mod", out)

    def test_with_sw3_off_a_song_that_stops_itself_is_not_followed(self):
        # F00 stops a Song whatever SW3 says; only SW3 moves on to the next
        s = modgen.Song("stops")
        s.sample(1, modgen.sine_cycle(32) * 8, volume=50, loop=(0, 256), name="sine")
        s.note(0, 0, 0, period=modgen.C2, sample=1, effect=0xF, param=1)
        s.note(0, 1, 1, effect=0xF, param=0)
        img = fatgen.Fat32Image()
        img.add_file("first.mod", s.build())
        img.add_file("second.mod", short_song("song two"))
        rc, out = run_card(img.build(), "next Song", modplayer.encode_card_play("first.mod"),
                           max_cycles=60000000)
        self.assertNotEqual(rc, 0, out)
        self.assertIn("OK stops", out)

    def test_with_sw3_off_the_song_loops_and_nothing_follows(self):
        img = fatgen.Fat32Image()
        img.add_file("first.mod", short_song("song one"))
        img.add_file("second.mod", short_song("song two"))
        rc, out = run_card(img.build(), "next Song", modplayer.encode_card_play("first.mod"),
                           max_cycles=60000000)
        self.assertNotEqual(rc, 0, out)                          # not seen: it kept looping
        self.assertIn("OK song one", out)

    def test_without_a_card_the_commands_say_so(self):
        args = [SOC_TB, "--max", "200000000", "--after", "ready", "--send"]
        tmp = tempfile.mkdtemp()
        sp = os.path.join(tmp, "send.bin")
        with open(sp, "wb") as f:
            f.write(modplayer.encode_card_play("x.mod"))
        result = subprocess.run(args + [sp, "ERR no SD card"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
