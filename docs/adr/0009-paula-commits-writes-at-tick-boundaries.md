# Paula applies register writes at tick boundaries and pauses for memory

The soft CPU takes tens of microseconds to make a tick's register writes, and SDRAM fetches take a variable time; if Paula acted on writes as they arrived and on data whenever it came, its output would depend on CPU and memory timing and could not match the C model (ADR-0005). So Paula queues register writes and applies a whole tick's writes at once, at the next tick boundary, and Paula itself keeps the tick timer (the firmware tells it each tick's length). Its colour-clock engine pauses while it waits for Sample data and then catches up, so the sequence of output samples is exact and only their real-time placement wobbles, by microseconds.

## Consequences

- The firmware's loop is: wait for Paula's tick flag, run one Replayer tick (its writes queue up), write the next tick's length.
- A tick's writes must all be made before the next boundary; the Replayer needs well under 1 ms, against a shortest tick of about 10 ms.
- Audio is one tick (about 20 ms) behind the CPU's view of the Song, which is inaudible.
- The Replayer never writes Sample memory itself: invert loop (EFx) sends Paula an INVERT command (ProTracker's `-1 - x` flips every bit), which Paula applies at the boundary with the tick's other writes. Writing memory directly would land mid-tick, at a CPU-dependent moment; the Verilog Paula test caught exactly that.
