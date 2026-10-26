# Running it on the miniSpartan6+

Brought up on a real board on 2026-10-25 (Windows 11, ISE 14.7): it builds, boots (`sdram ok`, `paula ok`, `ready`) and plays `gods.mod`. This guide says how to build and use it, and what to look at first if it doesn't work.

## What you need

- **Xilinx ISE 14.7** (WebPACK, free licence; covers the XC6SLX25), on Windows or Linux. Only its command-line tools are used.
- **openFPGALoader**, which programs the FPGA through the board's own FT2232H (its first channel is wired as JTAG), so no separate cable is needed.
- **Python 3 with pyserial** (`pip install pyserial`) for `tools/modplayer.py`.
- Only if you change the firmware: **the RISC-V GCC toolchain** (Ubuntu/Debian: `apt install gcc-riscv64-unknown-elf`). A ready-built copy is kept in `hw/board/minispartan6/firmware/`: the boot loader that goes into the FPGA image (`boot0..3.hex`) and the firmware it loads (`firmware.bin`). `make board-firmware` refreshes them and `make test` fails if they are out of date.

## Build and program on Windows

1. **Build.** Open a Command Prompt in `hw\board\minispartan6` and run `build.bat`. It finds ISE in `D:\Xilinx\14.7\ISE_DS` or `C:\Xilinx\14.7\ISE_DS` (or set `ISE_DIR` to the folder holding `settings64.bat`), runs XST, ngdbuild, map, par, trce and bitgen, and writes `build\top.bit`. A build takes a few minutes.
2. **Install openFPGALoader.** The simplest route is MSYS2 (https://www.msys2.org): in its UCRT64 shell, `pacman -S mingw-w64-ucrt-x86_64-openFPGALoader`, then add `C:\msys64\ucrt64\bin` to the Windows PATH.
3. **Give the JTAG channel a USB driver.** Windows binds both FT2232H channels to FTDI's serial driver; openFPGALoader needs the first one on WinUSB. Run Zadig (https://zadig.akeo.ie), tick *Options > List All Devices*, pick the board's **Interface 0** and install **WinUSB**. Leave **Interface 1** alone: it stays the COM port the player talks over.
4. **Program.** `build.bat prog` loads the FPGA (lost at power-off). The image holds only the boot loader; the first time, give it the firmware with `modplayer.py firmware --save` (see Firmware and the boot loader below). To have the image load at power-on, write it to the SPI flash with `modplayer.py flash-write` (see SPI flash below).

### ISE 14.7 on Windows 10/11

What it took on a fresh Windows 11 install:

- **Every ISE tool hangs at start-up** (no output, no CPU, even `xst -help`) if WSL is installed. ISE's `libPortability.dll` lists network connections while it is still loading, and WSL's network provider (`P9NP`) deadlocks on that. Fix: remove `P9NP` from `ProviderOrder` under `HKLM\SYSTEM\CurrentControlSet\Control\NetworkProvider\Order` (e.g. `RDPNP,LanmanWorkstation,webclient`); export the key first to undo it. Windows or WSL updates may put it back. The ISE installer's hang at "enable WebTalk" (`xwebtalk.exe`) is the same deadlock; end that process and the install finishes.
- Copy `libPortabilityNOSH.dll` over `libPortability.dll` in both `ISE\lib\nt64` and `common\lib\nt64` (keep the originals). This is the well-known fix for the 64-bit GUI tools' crashes.
- Install the VC++ 2008 runtimes from `common\bin\nt64\vcredist_x64.exe` and `common\bin\nt\vcredist_x86.exe` (the licence manager needs them). The tools then still print "VC++ 2008 runtime libraries are not installed"; that warning is wrong and harmless.
- The licence must include the `ISE_WebPACK` feature: a Vivado licence doesn't cover ISE. Generate a free *ISE WebPACK* node-locked licence at AMD's licensing site and save it next to any other `.lic` in the folder `XILINXD_LICENSE_FILE` points to.
- `map` sometimes dies part-way through placement with no message; `build.bat` retries it.
- From Git Bash, run `build.bat` by its full path: bare names in the current directory aren't searched when `NoDefaultCurrentDirectoryInExePath` is set.

## Build and program on Linux

```sh
cd hw/board/minispartan6
source /opt/Xilinx/14.7/ISE_DS/settings64.sh   # wherever ISE is installed
make            # -> build/top.bit (builds the firmware first: needs RISC-V GCC)
make prog       # load into the FPGA (lost at power-off)
make flash      # write it and the firmware to the SPI flash, so they load at power-on
```

You may need a udev rule (or `sudo`) for access to the FTDI device.

## Notes on the build

Check the timing summary the build prints (from `build/top.twr`): the whole design is one 50 MHz clock (20 ns). The part is assumed to be `xc6slx25-ftg256-3`; if your chip says `-2`, change `PART` in the Makefile and `build.bat`.

Changing the firmware doesn't need an ISE build: the image holds only the boot loader, which loads the firmware from flash or over serial (next section). Rebuild the image only when the Verilog or the boot loader changes.

## Firmware and the boot loader

The FPGA image's RAM holds only a small boot loader (ADR-0011). At power-on it prints `boot loader 0.1`, waits 200 ms for the PC, then runs the firmware saved in flash (`firmware from flash`). With none saved, or a damaged copy, it says so and waits for the PC; LED 7 stays on while the boot loader is waiting.

```sh
python3 tools/modplayer.py --port COM7 firmware hw/board/minispartan6/firmware/firmware.bin          # run it now
python3 tools/modplayer.py --port COM7 firmware --save hw/board/minispartan6/firmware/firmware.bin   # and keep it in flash
```

`firmware` sends `B` (the running firmware returns to the boot loader; no power cycle needed), then the image, and waits for `ready`: about a second plus the SDRAM test. Without `--save`, the next power-on brings back the firmware in flash. A freshly built `build/fw/firmware.bin` works the same way.

If firmware in flash is broken and doesn't answer `B`: start `modplayer.py firmware --save` with good firmware, and while it is waiting (it keeps sending `B` for 20 seconds) reload the image over JTAG (`build.bat prog`, or `openFPGALoader -b minispartan6 -r`). The boot loader restarts, sees the `B` inside its 200 ms window and stays, and the good firmware replaces the broken one.

## Use

1. After programming, the boot loader prints `boot loader 0.1` and, after 200 ms, `firmware from flash` (LED 7 lights while it waits; if there is no firmware in flash yet, give it some: Firmware and the boot loader, above). Then LED 0 lights and blinks about three times a second (the heartbeat: the soft CPU is running). The firmware tests the SDRAM and checks that Paula's tick timer fires, printing over serial: `mod-player 0.1`, `sdram ok`, `paula ok`, `ready`.
2. The serial port is the FT2232H's **second** interface (`/dev/ttyUSB1` on Linux; on Windows the COM port Device Manager lists, e.g. `--port COM5`), at **1 000 000 baud**.
3. Plug an amplifier or powered speakers into the jack. **Start with the volume low:** the jack is driven straight from the FPGA pins through an RC filter, at up to about 3 V peak to peak. Headphones work but are quiet: the filter's 3.3 kΩ in series with 32 Ω leaves about 1% of the signal.
4. Load and play a Song:

   ```sh
   python3 tools/modplayer.py --port /dev/ttyUSB1 version
   python3 tools/modplayer.py --port /dev/ttyUSB1 load tests/songs/gods.mod
   python3 tools/modplayer.py --port /dev/ttyUSB1 info
   python3 tools/modplayer.py --port /dev/ttyUSB1 pause    # or stop, play
   ```

**DIP switches** (read when a Song starts): SW1 stereo blend, SW2 Amiga 500 filter, SW3 play once (stop at the end instead of looping), SW4 reserved (developer mode).

**LEDs:** LED 0 blinking: idle and alive. While playing, two per voice as a rough level meter (voice 1 = LEDs 0–1). LEDs alternating 10101010 / 01010101: the SDRAM test failed. No LEDs at all: the FPGA isn't configured or the clock isn't running.

## HDMI (sound on a TV or monitor)

The HDMI build adds 640×480 at 60 Hz output on the HDMI connector (J3) with the Song's sound as 48 kHz stereo. The picture is a text console (53 × 20 characters in the Spleen 12×24 font, 16 colours; ADR-0012): at power-on the firmware shows a test screen with the self-test results, every character and every colour (it should look like `tests/golden/console-test-screen.png`), and `T` writes text to it (`docs/protocol.md`). Once a Song is loaded the status screen replaces it: the Song's name, where it is (Position, Pattern, Row), speed and BPM, following the Song row by row, an oscilloscope trace for each of the four Voices (ADR-0013), and a 48-bar spectrum analyser of the mix with falling peak markers (ADR-0014). The audio jack keeps working at the same time. How the sound works is in ADR-0010.

- Windows: `build.bat hdmi` (or `build.bat hdmi prog`; to keep it at power-on, see SPI flash below). The image goes to `build-hdmi\top.bit`, so the plain build in `build\` is left alone.
- Linux: `make HDMI=1` (and `make HDMI=1 prog`), into `build-hdmi/`.

Use it as above, with the TV's or monitor's speakers instead of the jack. HDMI sound is about 0.7 ms behind the jack, and silence goes out whenever no Song is playing.

If the screen stays black or says "no signal":

1. Check `build-hdmi/top.twr`: there are now three clocks (50, 25.2 and 126 MHz), and all of them must meet timing. The 25.2 MHz pixel clock comes from a DCM (50 × 21/25 = 42 MHz) feeding a PLL (× 15 = 630 MHz, ÷ 5 and ÷ 25).
2. Some displays don't accept 640×480 over HDMI, even though it is the one format every HDMI sink must support. Try another display, or a monitor with a DVI input via an adapter (it shows the picture and ignores the sound).
3. Picture but no sound: the display may not expect audio at 640×480. ADR-0010's last point names a detail of the data packets (channel 0 bit 3) worth flipping in `hw/rtl/hdmi_tx.v` if a display is fussy.

## SPI flash

The board's configuration flash is a Macronix MX25L6406 (8 MB). `python3 tools/modplayer.py --port COM7 flash-id` asks the firmware to read its JEDEC ID through the SoC's own SPI master (pins T3, R11, T10, P10), and prints `OK flash c2 20 17`. That was first done on 2026-10-25 (issue #17).

**To make an image start at power-on**, write it to flash through the running firmware (issue #18):

```sh
build.bat hdmi prog                                              # load it over JTAG first
python3 tools/modplayer.py --port COM7 flash-write hw/board/minispartan6/build-hdmi/top.bit
```

The firmware must be running (after `build.bat prog`, the boot loader runs the firmware from flash, or give it firmware first). This takes about 30 seconds for an 800 KB image, and the firmware reads it back and checks it. The board's mode pins are set to boot from SPI flash, so the image loads at the next power-on. To load it straight away, run `openFPGALoader -b minispartan6 -r`, which makes the FPGA reconfigure from flash. Any image with the current firmware can write any other image, so you can load the plain build and write the HDMI one, or the other way round.

**To have a Song play at power-on, with no PC:** `python3 tools/modplayer.py --port COM7 save song.mod`. `forget` removes it, and SW4 (developer mode) stops it playing at power-on without removing it. Where things live in the flash is in `docs/flash-layout.md`.

`build.bat flash` / `make flash` (openFPGALoader `-f`) don't work on this board. openFPGALoader has no SPI-over-JTAG bridge for the XC6SLX25 in FTG256. One built with ISE from openFPGALoader's own source (`xilinx_spiOverJtag.v`, `constr_xc6s_ftg256.ucf`) loads and answers, but reads the flash's ID as all 1s or all 0s, although our own design reads it fine on the same pins.

## If it doesn't work

In rough order of likelihood. Bring-up hit 1, 3 (in a different form) and 4; the notes say what the fixes were.

1. **XST rejects something.** The Verilog is plain Verilog-2001 and synthesises with Yosys for Spartan-6 (`make synth-check`), but XST has its own quirks. Found so far: under `` `default_nettype none `` every port needs an explicit `wire` (Verilator doesn't insist; XST does), and the setting carries into the files XST reads next, so reset it at the end of the file. Other places to look: `$readmemh` paths (the build copies the hex files next to the XST run) and initial values on registers.
   - **No 9K block RAMs.** If any memory lands in a RAMB8BWER, bitgen has to write a special bitstream format (Xilinx AR 39999). ISE 14.7's bitgen then often crashes ("Incorrect number of bits in bitstream for FDRI write"), and the bitstreams it did write left the CPU stuck after its first few instructions. `top.xcf` and a `ram_style` attribute in `paula.v` keep the small memories in logic; `build/top_map.mrp` should say "Number of RAMB8BWERs: 0".
2. **Timing fails at 50 MHz.** Likely long paths: Paula's output stage (`S_POST` in `hw/rtl/paula.v`: blend, then a 32×18 multiply-add for the A500 filter) and PicoRV32's ALU. Paula has spare clocks per colour clock (it uses 3–5 of 14), so its output stage can be split over more states without changing behaviour; `make hw-test` will prove it still matches.
3. **No serial output.** LED 7 on alone means the boot loader is waiting for firmware: `modplayer.py firmware --save`. If LED 0 stays on without blinking, the firmware has stopped (on bring-up, that was the 9K block RAM problem in 1). Otherwise, check you're on the second FTDI interface at 1 Mbaud. The FT2232H's channel B must be in serial (UART) mode, which is its default; if its EEPROM was set to FIFO mode, set it back with FTDI's FT_Prog or `ftdi_eeprom`.
4. **`sdram FAIL` (LEDs alternating).** The message gives the failing address, the word expected and the word read. The SDRAM is clocked with the inverse of the system clock (`ODDR2` in `top.v`), so the chip samples commands half a cycle after they change. With CAS latency 2, a read beat is driven from about 6 ns after the SDRAM edge one cycle after the READ until about 3 ns after the next edge, so the controller captures each beat on our *falling* edge 2.5 (then 3.5) clocks after the READ (`dq_fall` in `hw/rtl/sdram_ctrl.v`), and the SDRAM model in `hw/sim/sdram_model.h` presents read data the same way. Bring-up found the first version capturing a clock late (`expected dd1e16cb got dd1edd1e`: the low half held the second beat); the model had made the same mistake, so the simulation passed. If reads are unreliable again, the capture edge (or the clock phase) is the thing to adjust, and the model with it.
5. **Sound on the wrong side or missing.** `AUDIO2` (pin A8, jack tip) is left and `AUDIO1` (B8, ring) right, from the schematic; `make board-test` checks the design drives them that way.

## What the simulation already proves

- The Replayer matches ProTracker 2.3 (pt2-clone) register write for register write.
- The Verilog Paula matches the C model sample for sample, and keeps up at 50 MHz.
- The whole chip (CPU, firmware, SDRAM controller against a rule-checking SDRAM model, Paula) plays every test Song exactly as the PC reference does.
- The board top level boots through the real pin wiring (tristate data bus, inverted SDRAM clock) and drives sound to the left and right pins correctly.
- All 56 pin locations match Scarab's published pin file.
- HDMI: the Resampler's Verilog matches its C model output for output; a sink model decoding the transmitter's characters finds legal HDMI 1.3 timing, packets, checksums and InfoFrames; and Songs' sound comes out of the whole HDMI chain, across its clock domains, exactly as the C model computes it (`make resampler-test hdmi-test hdmi-chain-test`). The 8 HDMI pins are from Scarab's pin file too.
