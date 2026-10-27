# Songs are read from a FAT32 card's root directory, read-only

The firmware reads Songs from the microSD card (ADR-0015) with its own small FAT32 reader (`firmware/fat.c`), read-only and limited to the root directory. Cards are formatted on a PC, and FAT32 is what Windows, macOS and Linux all write for cards up to 32 GB; MOD files are small, so a flat folder of them is enough.

- **Finding the volume**: sector 0 is either a FAT32 boot sector itself (a card formatted without a partition table) or an MBR whose partitions of type 0x0B, 0x0C (or their hidden forms 0x1B, 0x1C) are tried in turn. A boot sector counts as FAT32 by its BIOS parameter block (512-byte sectors, no fixed root directory, no FAT16 size, a FAT32 size), not by its count of clusters, so small test images work too.
- **Names**: a file is found by its long name or its 8.3 name, ignoring case (ASCII only: other characters in long names never match). Deleted entries, directories and the volume label are skipped; a long name counts only if its checksum matches the 8.3 entry after it.
- **Reading**: the cluster chain is followed through the FAT (one FAT sector cached), and whole sectors go straight into SDRAM. A chain that leaves the volume, ends before the file does, or is longer than the volume has clusters (a loop) is an error, as is a file larger than the Song memory. Errors are messages, never a crash.
- **Commands**: `N` lists the root directory's files with their sizes, `K` gives a file's size and CRC-32 (to check it against the PC), `R` loads a Song and plays it (`modplayer.py card-ls`, `card-crc`, `card-play`).
- **SW3 (play once) moves on**: with SW3 on, a Song from the card that ends (or stops itself with F00) is followed by the next Song file in the root directory, in directory order, wrapping round to the first. Song files are those whose names end in `.mod` or start with `mod.` (the Amiga's naming); one that doesn't load is passed over. Songs from the PC or flash stop at their end, as before. At the owner's request; SW3 was chosen since "play once" and "then the next" go together.
- **At power-on**, if no Song is saved in flash (or the saved one can't play) and SW4 (developer mode) is off, the card's `default.mod` plays if there is one. The saved Song comes first because saving one is a deliberate act; `modplayer.py forget` hands power-on back to the card. The board says `ready` only after this: bytes the PC sends while it loads a Song would be lost.

On the board, a card formatted by Windows read correctly at once: every file's CRC matched the PC's, the largest (365 KB, 714 blocks in one go) included. Long reads had reset the board on PC USB power before (#25); Songs of this size did not.

## Considered options

- **FatFs (ChaN's FAT library)**: complete and widely used, but large for 60 KB of firmware RAM, and writing, subdirectories and exFAT aren't needed.
- **exFAT**: what Windows uses for cards over 32 GB; its specification is now open, but its directory format is more work, and cards that large can be formatted FAT32 with other tools.
- **Subdirectories and browsing on screen**: left for later; the root directory is enough to choose a Song by name from the PC.
- **A dedicated card instead of the PC's format**: no PC tool could write it.

## Consequences

- `tools/fatgen.py` makes FAT32 images (with and without an MBR, long names, fragmented files, many-cluster root directories, damaged chains) for `tests/test_fat.py`, which runs them through the SD card model in the SoC simulation. The images are this project's own reading of the specification; the board test with a Windows-formatted card is the independent check.
- A Song read from the card stops the one playing, as a load from the PC does.
