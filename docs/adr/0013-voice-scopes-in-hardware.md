# The Voice scopes are captured and drawn by hardware, triggered like a real scope, with fading persistence

The status screen shows four oscilloscope traces, one per Voice, in a 2 x 2 grid in rows 4-11 of the console (y 96..287), each 316 pixels wide and 96 tall (`hw/rtl/scopes.v`). (The first version had them four across, 155 wide and 192 tall; the owner found them too low resolution, and wide matters more than tall.) They come from Paula's own per-Voice output (each Voice's sample x volume, the values its mix is made of), copied to an output port when Paula mixes, so its audio is unchanged.

Once a frame each Voice waits for its output to cross zero going up, as an oscilloscope triggers, so a steady note stands still on screen; if none comes within 10 ms (notes below about 99 Hz) it starts anyway. It then takes 316 points, one every 64 colour clocks (5.7 ms: several cycles of a typical note), each `(output >> 7) * 0.75` so a full-scale Voice fills the ±48-pixel band; points are stored in 7 bits. The longest wait plus a capture still fits in a video frame, so the scopes update every frame. Consecutive points are joined by vertical runs, so the trace is a continuous line.

At the owner's request the traces persist like a scope's phosphor: each Voice keeps a ring of 4 capture slots, and the screen shows the newest capture at full brightness and the two before it at half and a quarter (about 50 ms of trail at 60 frames a second). The fourth slot is the one being captured into. When all four Voices finish a capture its slot becomes the newest; the screen takes it at the next frame, and a new capture starts only once the screen has taken the last one (the slot numbers cross the clock domains Gray coded), so the slot being written is never on screen. A column's four slots are read together, so drawing three generations costs no more reads than one. The firmware turns the scopes on with bit 0 of the console's control register, while the status screen is up, and labels them with their Voice numbers in the text underneath.

## Considered options

- **Firmware draws the scopes in text cells** (block characters): a 12 x 24 cell gives only a few levels, the CPU would have to copy hundreds of samples a frame across the bus, and the Replayer's timing has to come first.
- **One shared capture memory in block RAM**: all four Voices capture on the same colour clock, so one write port would need queuing; sixteen small distributed RAMs (Voice x slot, 512 x 7 each) take about 1500 LUTs, which the device has to spare (the analyser uses most of the block RAM).
- **Free-running capture (no trigger)**: simpler, but the traces roll and flicker.
- **A phosphor bitmap that fades pixel by pixel**: smoother decay, but 4 x 316 x 96 pixels at even 2 bits each is more block RAM than is left.

## Consequences

- `make console-test` checks every captured point against the reference Paula C model's Voice output, taken at a rising zero crossing (or after the time-out) and every 64 frames after, and every frame's scope pixels against a model of the drawing, all three generations. `scopes_tb` stalls Paula's frames at random and lands captures just before the screen's hand-over, checking no capture ever writes a slot on screen and that captures keep pace with the frames.
- Scope traces are drawn over the console's text; anything the firmware puts in rows 4-11 shows only where there is no trace.
- While Paula is paused the capture waits, so the scopes freeze on the last traces.
- The slot memories carry `ram_style = "distributed"`: left to itself XST put seven of them in RAMB8BWERs, which ISE 14.7's bitgen mishandles (docs/board.md), and `build.bat` stopped the build.
