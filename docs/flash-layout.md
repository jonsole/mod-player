# SPI flash layout

The board's flash is a Macronix MX25L6406: 8 MB in 4 KB sectors (ADR-0006, issues #18 to #20). `firmware/flash.h` and `tools/modplayer.py` both hold these numbers, and `tests/test_soc.py` checks that they agree.

| Address | Size | Holds | Written by |
| --- | --- | --- | --- |
| `0x000000` | 1 MB | FPGA image: an ISE `.bit` file's configuration data. The FPGA loads it at power-on; the XC6SLX25's is 801 KB | `modplayer.py flash-write top.bit` |
| `0x100000` | 1 MB | Firmware, for the boot loader (issue #20); unused for now | |
| `0x200000` | 6 MB | Saved Song, played at power-on | `modplayer.py save song.mod`; `modplayer.py forget` removes it |

## Saved Song

A 16-byte header, then the MOD file:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `MODS` |
| 4 | 4 | length of the MOD file in bytes (little-endian; at most 6 MB minus 16) |
| 8 | 4 | CRC-32 of the MOD file (as in `docs/protocol.md`) |
| 12 | 4 | zero |
| 16 | length | the MOD file |

After its self-tests (once it has printed `ready`), the firmware reads the header. With no `MODS` there it does nothing. Otherwise it copies the Song into SDRAM, checks the CRC and plays it, printing `saved Song: <name>`. A wrong CRC prints `saved Song damaged: ignored`. With SW4 (developer mode) on, it prints `saved Song not played: SW4 is on` and leaves the board to the PC. Reading takes about 0.6 s for a 300 KB Song.
