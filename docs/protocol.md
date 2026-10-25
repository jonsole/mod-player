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
| Flash write | `W`, address (u32), length (u32), the data, CRC-32 of the data (u32). Stops the Song; the address must be a multiple of 4096. The board erases the 4 KB sectors (64 KB blocks where it can) the data covers, programs it and reads it back | `OK written`, or `ERR <reason>` (checksum mismatch, address, past the end of the flash, or `flash doesn't read back as written`) after the whole transfer. Erasing takes up to a few seconds per 64 KB, so allow time |
| Flash CRC | `C`, address (u32), length (u32) | `OK crc <8 hex digits>`: CRC-32 of that range of flash |

Saving a Song to play at power-on is a flash write of a saved-Song record to its region (`docs/flash-layout.md`). At power-on, after `ready`, the board prints `saved Song: <name>` when it starts playing one.

CRC-32 is the zlib/PNG one (polynomial 0xEDB88320, reflected, initial and final XOR 0xFFFFFFFF). If more than 100 ms pass between bytes of a command, the board replies `ERR timeout` and waits for a new command. Wait for each reply before sending the next command: the board's serial input holds only one byte, so bytes sent while it is busy (erasing flash, for example) are lost. The largest Song is 32 MB minus 128 KB (the end of SDRAM is reserved for Paula's silence buffer).
