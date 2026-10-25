# Running it on the miniSpartan6+

Everything below the FPGA tools has been tested in simulation; **nothing has yet been built with Xilinx ISE or run on a real board.** This guide says how to try, and what to look at first if it doesn't work.

## What you need

- **Xilinx ISE 14.7** (WebPACK, free licence; covers the XC6SLX25), on Windows or Linux. Only its command-line tools are used.
- **openFPGALoader**, which programs the FPGA through the board's own FT2232H (its first channel is wired as JTAG), so no separate cable is needed.
- **Python 3 with pyserial** (`pip install pyserial`) for `tools/modplayer.py`.
- Only if you change the firmware: **the RISC-V GCC toolchain** (Ubuntu/Debian: `apt install gcc-riscv64-unknown-elf`). A ready-built copy of the firmware is kept in `hw/board/minispartan6/firmware/`; `make board-firmware` refreshes it and `make test` fails if it is out of date.

## Build and program on Windows

1. **Build.** Open a Command Prompt in `hw\board\minispartan6` and run `build.bat`. It finds ISE in `D:\Xilinx\14.7\ISE_DS` or `C:\Xilinx\14.7\ISE_DS` (or set `ISE_DIR` to the folder holding `settings64.bat`), runs XST, ngdbuild, map, par, trce and bitgen, and writes `build\top.bit`. A build takes a few minutes.
2. **Install openFPGALoader.** The simplest route is MSYS2 (https://www.msys2.org): in its UCRT64 shell, `pacman -S mingw-w64-ucrt-x86_64-openFPGALoader`, then add `C:\msys64\ucrt64\bin` to the Windows PATH.
3. **Give the JTAG channel a USB driver.** Windows binds both FT2232H channels to FTDI's serial driver; openFPGALoader needs the first one on WinUSB. Run Zadig (https://zadig.akeo.ie), tick *Options > List All Devices*, pick the board's **Interface 0** and install **WinUSB**. Leave **Interface 1** alone: it stays the COM port the player talks over.
4. **Program.** `build.bat prog` loads the FPGA (lost at power-off); `build.bat flash` writes the SPI flash so it loads at power-on.

ISE 14.7 on Windows 10/11 is known for crashes in its graphical tools (Project Navigator, iMPACT) under 64-bit; the command-line tools used here are not affected.

## Build and program on Linux

```sh
cd hw/board/minispartan6
source /opt/Xilinx/14.7/ISE_DS/settings64.sh   # wherever ISE is installed
make            # -> build/top.bit (builds the firmware first: needs RISC-V GCC)
make prog       # load into the FPGA (lost at power-off)
make flash      # write to the SPI flash, so it loads at power-on
```

You may need a udev rule (or `sudo`) for access to the FTDI device.

## Notes on the build

Check the timing summary the build prints (from `build/top.twr`): the whole design is one 50 MHz clock (20 ns). The part is assumed to be `xc6slx25-ftg256-3`; if your chip says `-2`, change `PART` in the Makefile and `build.bat`.

Changing the firmware means re-running the ISE build, because the firmware lives in the FPGA's block RAM. (A boot loader that loads firmware over serial or from flash is planned, ADR-0006.)

## Use

1. After programming, LED 0 lights, then blinks about three times a second (the heartbeat: the soft CPU is running). The firmware tests the SDRAM and checks that Paula's tick timer fires, printing over serial: `mod-player 0.1`, `sdram ok`, `paula ok`, `ready`.
2. The serial port is the FT2232H's **second** interface (`/dev/ttyUSB1` on Linux; on Windows the COM port Device Manager lists, e.g. `--port COM5`), at **1 000 000 baud**.
3. Plug headphones or an amplifier into the jack. **Start with the volume low:** the jack is driven straight from the FPGA pins through an RC filter, at up to about 3 V peak to peak.
4. Load and play a Song:

   ```sh
   python3 tools/modplayer.py --port /dev/ttyUSB1 version
   python3 tools/modplayer.py --port /dev/ttyUSB1 load tests/songs/gods.mod
   python3 tools/modplayer.py --port /dev/ttyUSB1 info
   python3 tools/modplayer.py --port /dev/ttyUSB1 pause    # or stop, play
   ```

**DIP switches** (read when a Song starts): SW1 stereo blend, SW2 Amiga 500 filter, SW3 play once (stop at the end instead of looping), SW4 reserved (developer mode).

**LEDs:** LED 0 blinking: idle and alive. While playing, two per voice as a rough level meter (voice 1 = LEDs 0–1). LEDs alternating 10101010 / 01010101: the SDRAM test failed. No LEDs at all: the FPGA isn't configured or the clock isn't running.

## If it doesn't work

In rough order of likelihood:

1. **XST rejects something.** The Verilog is plain Verilog-2001 and synthesises with Yosys for Spartan-6 (`make synth-check`), but XST has its own quirks. The places to look first: `$readmemh` paths (the Makefile copies the hex files next to the XST run), initial values on registers (used for power-up states), and PicoRV32 itself (used with ISE by others, but not here).
2. **Timing fails at 50 MHz.** Likely long paths: Paula's output stage (`S_POST` in `hw/rtl/paula.v`: blend, then a 32×18 multiply-add for the A500 filter) and PicoRV32's ALU. Paula has spare clocks per colour clock (it uses 3–5 of 14), so its output stage can be split over more states without changing behaviour; `make hw-test` will prove it still matches.
3. **No serial output.** Check you're on the second FTDI interface at 1 Mbaud. The FT2232H's channel B must be in serial (UART) mode, which is its default; if its EEPROM was set to FIFO mode, set it back with FTDI's FT_Prog or `ftdi_eeprom`.
4. **`sdram FAIL` (LEDs alternating).** The SDRAM is clocked with the inverse of the system clock (`ODDR2` in `top.v`), so the chip samples commands half a cycle after they change. Read data arrives roughly 2–16 ns after one of our rising edges (6 ns access time plus pin delays), so the controller captures it on the *falling* edge (`dq_fall` in `hw/rtl/sdram_ctrl.v`), mid-window, and the testbenches present read data only around that edge. If reads are still unreliable, that capture edge (or the clock phase) is the thing to adjust, and the testbench timing with it.
5. **Sound on the wrong side or missing.** `AUDIO2` (pin A8, jack tip) is left and `AUDIO1` (B8, ring) right, from the schematic; `make board-test` checks the design drives them that way.

## What the simulation already proves

- The Replayer matches ProTracker 2.3 (pt2-clone) register write for register write.
- The Verilog Paula matches the C model sample for sample, and keeps up at 50 MHz.
- The whole chip (CPU, firmware, SDRAM controller against a rule-checking SDRAM model, Paula) plays every test Song exactly as the PC reference does.
- The board top level boots through the real pin wiring (tristate data bus, inverted SDRAM clock) and drives sound to the left and right pins correctly.
- All 56 pin locations match Scarab's published pin file.
