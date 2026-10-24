# Amiga MOD player for the Scarab miniSpartan6+

A standalone player for Amiga ProTracker MOD files on the Scarab miniSpartan6+ (XC6SLX25) FPGA board. A RISC-V soft CPU runs a ProTracker 2.3-accurate Replayer and drives a hardware recreation of the Amiga's Paula audio chip.

- Design: [spec issue](https://github.com/jonsole/mod-player/issues/1), `GLOSSARY.md`, decisions in `docs/adr/`.
- Status: phase 1 (PC reference) done; phase 2 (full-chip simulation) in progress.

## Layout

- `firmware/replayer/`: the Replayer and MOD loader. Plain C with no C library, built for both the soft CPU and the PC.
- `host/`: PC-only code: the Paula C model, WAV writer and `render` command.
- `tools/modgen.py`: builds MOD files for tests; run it to write the generated test Songs.
- `tools/modplayer.py`: the PC side of the serial protocol (`docs/protocol.md`): load, play, stop, pause, info.
- `hw/rtl/`: the FPGA design in Verilog-2001 (Paula so far).
- `hw/sim/`: Verilator testbenches.
- `tests/`: unit tests (C), render-command tests, and the pt2-clone oracle (`tests/oracle/`, test-only, BSD-3).

## Tests and tools

Needs a C compiler, `make`, Python 3 and Verilator (`apt install verilator`). `make test` runs everything, including the Verilog Paula against the C model (`make hw-test` alone).

```sh
make test                                   # build and run every test
build/render song.mod song.wav --play-once  # render a whole Song (or --seconds N)
build/render song.mod song.wav --a500 --blend  # A500 filter, stereo blend (--no-led too)
python3 tools/modgen.py build/songs         # write the generated test Songs

# Compare our Replayer with pt2-clone's, Paula write by Paula write:
build/pt2_log song.mod 3000 > theirs.log
build/replayer_log song.mod 3000 > ours.log
python3 tools/compare_logs.py theirs.log ours.log
```
