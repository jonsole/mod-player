# Amiga MOD Player

A standalone player for Amiga ProTracker music files, built on the Scarab miniSpartan6+ FPGA board. It recreates the Amiga's split between a CPU running replay software and a dedicated audio chip.

## Playback

**Paula**:
The hardware audio block inside the FPGA: four independent sample-playback voices, modelled on the Amiga chip of the same name.
_Avoid_: sound chip, mixer, audio engine

**Voice**:
One of Paula's four playback units, each with its own sample pointer, length, period and volume.
_Avoid_: channel (reserved for the tracker column it plays), track

**Replayer**:
The software, running on the soft CPU, that reads the song each tick and programs Paula's voices.
_Avoid_: player routine, driver, sequencer

**Tick**:
One step of the Replayer's clock, normally 1/50 s; several ticks make one row.
_Avoid_: frame, interrupt

**Period**:
The value that sets a voice's playback rate, counted in Amiga colour-clock cycles per sample. Lower period means higher pitch.
_Avoid_: frequency, pitch value

## Board

**Soft CPU**:
The small RISC-V processor built from FPGA logic that runs the Replayer and loader.
_Avoid_: MCU, core, processor
