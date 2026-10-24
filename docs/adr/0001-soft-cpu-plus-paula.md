# Soft CPU runs the Replayer; Paula is hardware

We split the player the way a real Amiga does: a small RISC-V soft CPU runs the Replayer in C, and a hardware Paula block plays the four voices. Writing all of ProTracker's effects (vibrato, portamento, pattern loops, delays and their quirks) as Verilog state machines would be slow to write and hard to debug, while sample playback is the part that truly needs hardware timing. The cost is a CPU, firmware toolchain and memory bus in the FPGA.

## Considered Options

- **All-hardware sequencer**: no CPU or compiler, but every effect becomes a state machine; rejected for effort and debuggability.
