# The boot loader lives in the top 4 KB of a 64 KB firmware RAM

ADR-0006 says the FPGA image holds only a boot loader that runs the firmware from flash or from the PC. Where it lives: the firmware block RAM grows from 32 KB to 64 KB (32 of the XC6SLX25's 52 RAMB16s), the boot loader is linked at `0xF000` (the top 4 KB, its stack included) and the firmware at 0, with 60 KB for its code, data and stack. The firmware never touches the top 4 KB, so the boot loader is still there while the firmware runs, and the firmware's `B` command jumps back into it: new firmware can be sent without power-cycling the board or using JTAG.

At power-on the boot loader prints `boot loader <version>` and waits 200 ms for a byte from the PC. If none comes, it copies the firmware record from flash (`docs/flash-layout.md`: `MODF`, length, CRC-32, then the image) to address 0, checks the CRC and jumps there. Missing or damaged firmware leaves it waiting for the PC, which sends firmware with `X` (`docs/protocol.md`). Saving firmware to flash is done by the firmware itself, with the existing `W` command, so the boot loader only needs to read flash: it stays small (about 1.7 KB) and shares `serial.c`, `crc32.c` and `flash.c` with the firmware.

## Considered options

- **A separate boot ROM**: a small memory of its own would keep all 32 KB for the firmware, but memories that small tend to land in RAMB8BWERs, which ISE 14.7's bitgen mishandles (docs/board.md), and it adds a decoder and a read mux.
- **Boot loader in the top of the existing 32 KB, overwritten once the firmware runs**: no extra block RAM, but the firmware could not return to the boot loader, so every firmware change would need a power cycle inside the 200 ms window.
- **Running the firmware from SDRAM**: unlimited size, but every instruction fetch would compete with Paula's Sample fetches and slow the CPU several times over.

## Consequences

- The FPGA image is the boot loader alone (`hw/board/minispartan6/firmware/boot0..3.hex`); a new board needs the firmware once (`modplayer.py firmware --save firmware.bin`) before it does anything else.
- The simulations that test the firmware still start it directly from RAM (the SoC's `RESET_ADDR` parameter is 0 there); `tests/test_boot.py` runs the SoC as the FPGA image has it, reset address `0xF000`, with only the boot loader in RAM.
- Firmware is limited to 60 KB including its stack (today about 16 KB); the linker refuses code and data that don't fit.
- Firmware upload over serial and flash firmware go to the same place, so a firmware change takes about a second instead of an ISE rebuild.
