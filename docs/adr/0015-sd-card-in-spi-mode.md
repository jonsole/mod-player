# The microSD card is used in SPI mode, through a second SPI master

The board's microSD socket has all its lines wired to the FPGA: CLK, CMD, DAT0-DAT3 and no card-detect pin (CD shares DAT3). The card is used in **SPI mode**, not its native 4-bit mode: chip select on CD/DAT3, data out on CMD, data in on DAT0, clock on CLK. DAT1 and DAT2 are unused in SPI mode.

The SoC gets a second instance of the flash's SPI master (`hw/rtl/spi_master.v`, at `0x2000_0400`, same registers as the flash's at `0x2000_0300`). A card must be started at 100-400 kHz and can then run faster, so the master gains a slow mode, selected with bit 1 of the control register when the chip select is written: SCK = clock / 128 = 390 kHz instead of clock / 4 = 12.5 MHz. The driver (`firmware/sd.c`) sends 80 start-up clocks with the card deselected, then CMD0, CMD8 (to tell a version 2 card from version 1: a version 1 card answers "illegal command"), ACMD41 until the card is ready (up to a second; with the HCS bit set for version 2), CMD58 (the OCR's CCS bit tells SDHC, addressed by block, from standard capacity, addressed by byte; the latter also gets CMD16 to set 512-byte blocks), then switches to the fast clock. A block is CMD17, an R1 response, the data token 0xFE, 512 bytes and two CRC bytes (not checked: CRC is off in SPI mode).

SPI mode costs little: no new clock domain, about 100 LUTs, no extra pins beyond the four. The speed is about 600 KB/s with the CPU moving the bytes, so a 300 KB Song loads in about half a second.

The test oracle is a model of an SD card in SPI mode (`hw/sim/spi_sd_model.h`) that checks the rules the real card has: at least 74 clocks with chip select high before the first command, SCK no faster than 400 kHz until the card is ready, and the order of commands (CMD0 first, a read only when ready). It plays version 1 and 2, SDSC and SDHC cards, and cards slow to start.

## Considered options

- **Native SD mode (4-bit)**: up to 12 MB/s, but a command-and-data state machine with CRC7 and CRC16 in hardware, a second clock domain for the card clock at start-up, and bidirectional data pins. Not needed for loading a Song.
- **A bit-banged SPI port from the CPU**: no new hardware, but at best a few hundred KB/s with the CPU spending every cycle on it; the SPI master is smaller and already tested.
- **One SPI master shared with the flash (with a chip-select mux)**: saves a few LUTs but ties the two together (the flash's firmware commands would have to avoid the card's slow mode), and the pins differ anyway.

## Consequences

- The card's pins are in `top.ucf` from Scarab's pin file (CLK L12, CD/DAT3 K12, CMD K11, DAT0 M10, with a pull-up on DAT0).
- The firmware's `D` (start the card) and `E` (the CRC-32 of a range of blocks) commands let the PC check a card; reading Songs from a FAT32 card is issue #26.
- A block read stops the Song: the CPU is busy for the whole transfer.
