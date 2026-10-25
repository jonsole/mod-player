# Serial protocol

The PC talks to the board over the FT2232H's second port at **1 000 000 baud, 8N1**, no flow control. `tools/modplayer.py` implements the PC side.

Every command is one byte, sometimes followed by binary arguments (little-endian). Every reply is one or more text lines ending in `\r\n`; the last line starts with `OK` or `ERR`.

| Command | Bytes sent | Reply |
| --- | --- | --- |
| Version | `V` | `OK mod-player <version>` |
| Load and play | `L`, length (u32), the MOD file, CRC-32 of the file (u32) | `OK <Song name>` or `ERR <reason>` |
| Play from memory | `M`, length (u32): a MOD file already at the start of SDRAM | as Load |
| Play | `P`: restart the loaded Song from the beginning | `OK` |
| Stop | `S`: stop and silence | `OK` |
| Pause | `U`: pause or resume | `OK paused` / `OK playing` |
| Info | `I` | `song <name>`, `positions <n>`, one `sample <n> <name>` line per non-empty Sample, then `OK` |
| Flash ID | `F`: read the SPI flash's JEDEC ID | `OK flash <manufacturer> <type> <capacity>` in hex, e.g. `OK flash c2 20 17` (the board's Macronix MX25L6406); `00 00 00` or `ff ff ff` means no flash is answering |

CRC-32 is the zlib/PNG one (polynomial 0xEDB88320, reflected, initial and final XOR 0xFFFFFFFF). If more than 100 ms pass between bytes of a command, the board replies `ERR timeout` and waits for a new command. The largest Song is 32 MB minus 128 KB (the end of SDRAM is reserved for Paula's silence buffer).
