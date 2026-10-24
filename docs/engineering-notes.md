# Engineering notes

A running account of how the player was built: what was decided, how it is tested, and what went wrong along the way. Detail lives in the code, the decision records (`docs/adr/`) and the issues; this is the story for an engineer who wants the shape of it.

## The idea

Recreate how an Amiga plays a ProTracker MOD file, on a Scarab miniSpartan6+ (Xilinx Spartan-6 XC6SLX25, 32 MB SDRAM, FT2232H USB, 3.5 mm jack):

```
 PC ──USB serial──► soft CPU (PicoRV32, RISC-V) ──registers──► Paula (4 voices) ──► delta-sigma ──► jack
                     runs the Replayer in C            │  fetches Sample data
                                                       ▼
                                              32 MB SDRAM (the Song)
```

As on the Amiga, software (the *Replayer*) reads the Song once per *tick* (normally 1/50 s) and programs four hardware *voices*; the hardware plays raw 8-bit samples at a rate set by a *period* counted in the Amiga's 3.546895 MHz colour clock. The split is ADR-0001; why RISC-V rather than a 68000 is ADR-0008.

## How it was designed

The design was settled before any code, in rounds of "grilling" questions (the `mattpocock/skills` workflow, vendored in `.claude/skills/`): vocabulary went into `GLOSSARY.md`, hard-to-reverse choices into ADRs, and the result into a spec issue and tickets. Some choices that shape everything:

- **Accuracy target: ProTracker 2.3, quirks included**, with no interpolation (ADR-0002). Only 4-channel, 31-sample MODs (ADR-0003).
- **Xilinx ISE 14.7** is the only toolchain for Spartan-6, so the HDL is plain Verilog-2001 (ADR-0004).
- **Simulation first, and exact**: the hardware must produce the same audio, sample for sample, as a PC reference (ADR-0005).

## Phase 1: a PC reference that matches ProTracker

**The Replayer** (`firmware/replayer/`, ~1000 lines of freestanding C) is a careful port of ProTracker 2.3's replay routine, structured like the original so behaviour and even the *order of register writes* match. It talks to Paula only through a tiny "Paula port" (a write function), with two adapters: the real registers on the board, and a C model on the PC.

**The Paula C model** (`host/paula_model.c`) steps once per colour clock: each voice counts down its period, fetches 16-bit words, plays bytes × volume, and on reaching the end reloads pointer and length from "latched" registers, which is how Amiga sample loops work. A `render` tool turns any MOD into a WAV.

**The test oracle** (ADR-0007) is the most useful single piece. pt2-clone, a well-regarded C port of ProTracker, has its replay code compiled for the PC with the editor/GUI stubbed out, and both replayers print a log of every Paula register write, tick by tick. The test demands the logs be identical. Twenty generated test Songs (one per effect group, written before the effect was implemented) and a real Song (`gods.mod`, 312 KB) all match for thousands of ticks, Sample memory included.

Things the oracle caught:
- **Tick length was one count short.** The Amiga's CIA timer fires on underflow, so a tick is `(1773447/BPM + 1) × 5` colour clocks (70940 at 125 BPM), not 70935.
- ProTracker's real quirks, now reproduced on purpose: arpeggio reading past the end of the period table at finetune −1, portamento up losing the sign of the period, the sample offset (9xx) being applied twice on a note, ramp tremolo reading the *vibrato* position.

Every ticket also got a **mutation check**: deliberately break the code a few plausible ways and confirm a test fails. That found weak tests twice (a tone-portamento test whose slides never reached their target, so the target-period quirk was never exercised; and an end-of-Song check that would have fired on every row after the first play-through).

**Paula's output options** are integer-only so the FPGA can match them exactly: a 25 % stereo blend, the A500's 4.4 kHz one-pole RC filter, and the "LED" filter as a two-pole state-variable filter (3.09 kHz, Q 0.66), with coefficients derived from the Amiga's component values and checked against analog filter theory.

## Phase 2: the chip, in simulation

Everything runs under Verilator in the normal test suite (`make test`, about a minute).

**Verilog Paula** (`hw/rtl/paula.v`) is compared against the C model frame by frame: same register writes in, same samples out, for every test Song (5 seconds each, all 8 option combinations) plus fuzzed random register writes, with the memory model answering after random delays.

Getting that exact needed a design decision (ADR-0009). On the board the CPU takes tens of microseconds to make a tick's ~40 register writes, and SDRAM reads take variable time; if Paula acted on writes as they trickled in, output would depend on CPU and memory timing. So:
- Paula **queues** register writes and applies a whole tick's worth at once, at the tick boundary. Paula keeps the tick timer itself; the firmware just waits for a "tick" flag, runs one Replayer tick and writes the next tick's length.
- Paula's colour-clock engine **pauses while waiting for memory, then catches up**, so the *sequence* of output samples is exact and only their real-time placement wobbles by microseconds.

The test then caught a subtler case: the invert-loop effect (EFx) makes the Replayer *modify Sample data*, and a CPU write to memory lands mid-tick at a CPU-dependent moment. Fix: ProTracker's `-1 - x` is just a bit flip, so it became a queued Paula command (`INVERT addr`), applied at the boundary like everything else (and order-independent).

Paula's engine was first written for clarity (13 clocks per colour clock, but only 14.1 are available at 50 MHz) and then reworked with the tests as a safety net: all four voices step in parallel, the output stage is merged, and it now takes 3 clocks (5 with the LED filter). A real-time build paced at 50 MHz confirms it never falls behind.

**Soft CPU**: PicoRV32 (rv32im) with 32 KB of block RAM, built as four byte-wide memories so XST can initialise them from `$readmemh` files. The firmware has its own start-up code and linker script; the testbench decodes the serial line.

**Delta-sigma DAC** (`hw/rtl/dac_dsm.v`): second order, input scaled by 13/16 for loop stability. The testbench runs the 50 MHz bitstream through a model of the board's RC filter (3.3 kΩ, 4.7 nF) and FFTs the audio band: 127 dB SNR at full scale, and no idle tones above −130 dBFS near silence, so no dither.

**SDRAM controller** (`hw/rtl/sdram_ctrl.v`): closed-page, every 32-bit access is ACTIVE then READ/WRITE with auto-precharge and a 2-beat burst; refresh every 7.4 µs. The SDRAM *model* (`hw/sim/sdram_model.h`) stores data and referees every datasheet rule (power-up wait and sequence, mode register, tRCD/tRP/tRC/tRAS/tWR/tRFC, refresh interval, bus contention), and drives garbage outside read windows so a capture one cycle off shows up as corrupt data. It caught a bug that would have hit real hardware: FPGA registers power up as 0, so before reset the SDRAM pins said "chip select, MODE REGISTER". The pin registers now have power-up values.
