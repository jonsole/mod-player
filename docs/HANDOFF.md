# Handoff: continuing in VS Code

Written 2026-10-25, at the end of the first (cloud) Claude Code session, so the work can carry on locally in VS Code with the Claude Code extension. If you are an agent picking this up, read this first, then `CLAUDE.md`, `GLOSSARY.md` and `docs/adr/`.

## Where things stand

| Phase | What | State |
|---|---|---|
| 1 | PC reference: Replayer (ProTracker 2.3 port in C), Paula C model, `render` to WAV, pt2-clone oracle | Done; matches pt2-clone write for write |
| 2 | The chip in simulation: Verilog Paula, PicoRV32 SoC, SDRAM controller, delta-sigma DAC, serial loading, full chip | Done; full chip matches the PC reference sample for sample |
| 3 | Board build: `top.v`, pins, ISE build (Makefile + `build.bat`) | Done 2026-10-25: built with ISE 14.7 on Windows 11; boots and plays `gods.mod` on the board (issue #16). Fixes and setup notes in `docs/board.md` |
| 5 | Boot loader, Song saved to flash (ADR-0006) | Not started |
| 6 | microSD | Not started |
| 7 | HDMI audio | Not started |

`docs/engineering-notes.md` tells the story of how it was built (published at https://claude.ai/artifact/1u8FxjXG6NGccab94wUaZw). `docs/board.md` is the build/program/use guide with troubleshooting.

## Git

- All work is on branch **`claude/amazing-turing-qjhzlh`** (pushed; nothing on `main` yet). Check it out: `git fetch && git checkout claude/amazing-turing-qjhzlh`.
- No pull request has been opened. Issues #1–#15 are complete in code but still open on GitHub; they would close naturally when this branch is merged (or close them by hand).
- Commit messages: imperative subject, short body explaining why.

## The immediate next step

Board bring-up (issue #16) is done: `build.bat prog` builds and loads the board, which prints `mod-player 0.1`, `sdram ok`, `paula ok`, `ready` and plays `gods.mod`. `docs/board.md` records what it took: ISE setup on Windows 11, keeping memories out of the 9K block RAMs, and the SDRAM read capture edge.

On the owner's PC: the player's serial port is COM7, openFPGALoader is in `C:\msys64\ucrt64\bin`, and WSL Ubuntu has the Linux tools. The Windows checkout has CRLF line endings, so run `make` in a clone inside WSL, not on `/mnt/c`.

Next is phase 7, HDMI with audio, started in a separate session. (The board's own audio jack is quiet into headphones: the filter's 3.3 kOhm is in series with them.)

When something fails on the board, the rule (ADR-0005) is: **reproduce it in simulation first**, fix it there with a test, then rebuild.

## Development environment

The tests need Linux tools. On Windows, use **WSL2** (Ubuntu) and open the repo with VS Code's *WSL* extension; ISE stays on Windows and reads the same files (e.g. clone into `\\wsl$\...` or into `/mnt/d/...`).

```sh
sudo apt install build-essential python3 verilator yosys gcc-riscv64-unknown-elf
make test            # everything, about 2.5 minutes
```

Verilator 5.x was used. Useful narrower targets:

| Target | What |
|---|---|
| `make test` | All of the below plus unit tests, render and oracle tests |
| `make hw-test` | Verilog Paula vs the C model (31 runs incl. fuzz and real-time) |
| `make soc-test` | Soft CPU + firmware + SDRAM model, memory test, serial loading |
| `make fullchip-test` | Whole chip vs PC reference, every test Song |
| `make dsm-test` | Delta-sigma DAC SNR and idle tones |
| `make board-test` | `top.v` with pin models: boots and drives left/right correctly |
| `make synth-check` | Yosys synthesis for Spartan-6, resource estimate (needs yosys) |
| `make firmware` | RISC-V firmware into `build/fw/` |
| `make board-firmware` | Refresh the ready-built firmware in `hw/board/minispartan6/firmware/` (commit it; `make test` fails if stale) |

Handy tools: `build/render song.mod out.wav` (PC reference audio), `tools/modplayer.py` (board CLI), `tools/modgen.py` (generates test Songs in Python), `tools/compare_logs.py`.

## Map of the code

| Path | What |
|---|---|
| `firmware/replayer/` | The Replayer: freestanding C, talks to Paula only through `paula_port.h` |
| `firmware/*.c` | Board firmware: `main.c` (boot, self-tests, command loop), `player.c`, `serial.c`, `crc32.c`, `memtest.c`, `hw.h` (address map) |
| `firmware/runtime/` | Start-up code, linker script, tiny libc |
| `host/` | PC side: Paula C model, register-write log, `render`, WAV writer |
| `hw/rtl/` | Verilog: `paula.v`, `soc.v`, `sdram_ctrl.v`, `dac_dsm.v`, UARTs, `bram.v`; PicoRV32 vendored in `third_party/` |
| `hw/sim/` | Verilator testbenches and the rule-checking SDRAM model |
| `hw/board/minispartan6/` | `top.v`, `top.ucf`, `Makefile`, `build.bat`, ready-built firmware |
| `tests/` | Unit tests, Python test drivers, pt2-clone oracle (`tests/oracle/`), `tests/songs/gods.mod` |
| `docs/` | ADRs, protocol, board guide, engineering notes, this file |

Key interfaces: Paula's register map is in `GLOSSARY.md` / ADR-0009 (voice registers 0x00–0x3C, DMACON 0x40, LED_FILTER 0x44, OPTIONS 0x48, INVERT 0x4C, then board-only TICK_LEN 0x50, STATUS 0x54, CONTROL 0x58). SoC address map: block RAM 0x0000_0000, SDRAM 0x1000_0000, UART 0x2000_0000, Paula 0x2000_0100, LEDs/switches 0x2000_0200. The serial protocol is `docs/protocol.md`.

## Rules worth keeping

- **Exactness is the point.** Hardware output must equal the PC reference sample for sample (ADR-0005), and the Replayer must equal pt2-clone write for write (ADR-0007). Don't loosen a comparison to make a test pass.
- **Paula applies register writes at tick boundaries** (ADR-0009). Anything that changes sound mid-tick (like the invert-loop effect) becomes a queued Paula command, not a direct memory write.
- **Test first** (`.claude/skills/tdd`), and after a fix, break the code on purpose to check the test catches it.
- Plain Verilog-2001 for XST; no SystemVerilog. Integer-only arithmetic in Paula so the C model and Verilog agree.
- Use the vocabulary in `GLOSSARY.md` (Song, Sample, tick, voice, period...) in code, docs and issues.
- Design discussions use the vendored skills in `.claude/skills/` (grilling, domain-modeling, to-spec, to-tickets, tdd, codebase-design).

## Open items for the owner

- Board bring-up (#16), above.
- Listen to `gods.mod` on the board, and supply more MOD files for end-to-end tests (#9).
- Decide when to merge the branch to `main`.
