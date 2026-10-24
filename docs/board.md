# Running it on the miniSpartan6+

Everything below the FPGA tools has been tested in simulation; **nothing has yet been built with Xilinx ISE or run on a real board.** This guide says how to try, and what to look at first if it doesn't work.

## What you need

- **Xilinx ISE 14.7** (WebPACK, free licence; covers the XC6SLX25). On Linux natively, or Xilinx's ISE 14.7 Windows 10 VM.
- **The RISC-V GCC toolchain** to build the firmware (Ubuntu/Debian: `apt install gcc-riscv64-unknown-elf`). The firmware can be built on any machine; ISE only needs the four `build/fw/firmware*.hex` files.
- **xc3sprog**, which programs the FPGA through the board's own FT2232H (its first channel is wired as JTAG).
- **Python 3 with pyserial** (`pip install pyserial`) for `tools/modplayer.py`.

## Build

```sh
make firmware                                  # in the repository root
cd hw/board/minispartan6
source /opt/Xilinx/14.7/ISE_DS/settings64.sh   # wherever ISE is installed
make                                           # -> build/top.bit
```

The build runs XST, ngdbuild, map, par, trce and bitgen. Check the timing summary it prints (from `build/top.twr`): the whole design is one 50 MHz clock (20 ns). The part is assumed to be `xc6slx25-ftg256-3`; if your chip says `-2`, change `PART` in the Makefile.

Changing only the firmware still means re-running the ISE build, because the firmware lives in the FPGA's block RAM. (A boot loader that loads firmware over serial or from flash is planned, ADR-0006.)

## Program

```sh
make prog     # load into the FPGA (lost at power-off)
make flash    # write to the SPI flash, so it loads at power-on
              # (needs xc3sprog's bscan_spi_xc6slx25.bit in this folder)
```

## Use

1. After programming, LED 0 lights. The firmware tests the SDRAM, then prints over serial: `mod-player 0.1`, `sdram ok`, `ready`.
2. The serial port is the FT2232H's **second** interface (`/dev/ttyUSB1` on Linux, the higher-numbered COM port on Windows), at **1 000 000 baud**.
3. Plug headphones or an amplifier into the jack. **Start with the volume low:** the jack is driven straight from the FPGA pins through an RC filter, at up to about 3 V peak to peak.
4. Load and play a Song:

   ```sh
   python3 tools/modplayer.py --port /dev/ttyUSB1 version
   python3 tools/modplayer.py --port /dev/ttyUSB1 load tests/songs/gods.mod
   python3 tools/modplayer.py --port /dev/ttyUSB1 info
   python3 tools/modplayer.py --port /dev/ttyUSB1 pause    # or stop, play
   ```

**DIP switches** (read when a Song starts): SW1 stereo blend, SW2 Amiga 500 filter, SW3 play once (stop at the end instead of looping), SW4 reserved (developer mode).

**LEDs:** while playing, two per voice as a rough level meter (voice 1 = LEDs 0–1). LEDs alternating 10101010 / 01010101 means the SDRAM test failed.

## If it doesn't work

In rough order of likelihood:

1. **XST rejects something.** The Verilog is plain Verilog-2001 and synthesises with Yosys for Spartan-6 (`make synth-check`), but XST has its own quirks. The places to look first: `$readmemh` paths (the Makefile copies the hex files next to the XST run), initial values on registers (used for power-up states), and PicoRV32 itself (used with ISE by others, but not here).
2. **Timing fails at 50 MHz.** Likely long paths: Paula's output stage (`S_POST` in `hw/rtl/paula.v`: blend, then a 32×18 multiply-add for the A500 filter) and PicoRV32's ALU. Paula has spare clocks per colour clock (it uses 3–5 of 14), so its output stage can be split over more states without changing behaviour; `make hw-test` will prove it still matches.
3. **No serial output.** Check you're on the second FTDI interface at 1 Mbaud. The FT2232H's channel B must be in serial (UART) mode, which is its default; if its EEPROM was set to FIFO mode, set it back with FTDI's FT_Prog or `ftdi_eeprom`.
4. **`sdram FAIL` (LEDs alternating).** The SDRAM is clocked with the inverse of the system clock (`ODDR2` in `top.v`), so the chip samples commands half a cycle after they change, and read data is captured 3 clocks after a READ (`S_READ` in `hw/rtl/sdram_ctrl.v`). If reads are unreliable, the capture point (or the clock phase) is the thing to adjust; the simulation model's timing will then need the same change.
5. **Sound on the wrong side or missing.** `AUDIO2` (pin A8, jack tip) is left and `AUDIO1` (B8, ring) right, from the schematic; `make board-test` checks the design drives them that way.

## What the simulation already proves

- The Replayer matches ProTracker 2.3 (pt2-clone) register write for register write.
- The Verilog Paula matches the C model sample for sample, and keeps up at 50 MHz.
- The whole chip (CPU, firmware, SDRAM controller against a rule-checking SDRAM model, Paula) plays every test Song exactly as the PC reference does.
- The board top level boots through the real pin wiring (tristate data bus, inverted SDRAM clock) and drives sound to the left and right pins correctly.
- All 56 pin locations match Scarab's published pin file.
