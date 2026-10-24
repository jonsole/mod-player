# Paula's hardware must match the PC reference model sample for sample

Everything is proven in simulation before it reaches the board. A PC build of the Replayer drives a C model of Paula to render WAV files, and the full-chip simulation (soft CPU, Paula, SDRAM model) must produce exactly the same output samples. This is why Paula's timing is defined in whole Amiga colour-clock steps and its mixing in integer arithmetic: anything non-deterministic or approximate would break the comparison that catches hardware bugs.
