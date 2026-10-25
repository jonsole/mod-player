# HDMI audio: an exact integer Resampler and clocks locked to the 50 MHz oscillator

HDMI carries audio as 48 kHz samples, but Paula produces one frame per colour clock (3 546 895 Hz) and the audio jack plays those directly. Dropping or averaging frames to get 48 kHz would fold Paula's ultrasonic images (a Sample played at 28 kHz has an image at 20 kHz once folded, only 6 dB down after simple averaging) back into the audible band. So a Resampler with a proper low-pass filter converts Paula's frames to 48 kHz: a 4th-order CIC decimating by 16, then a 128-tap polyphase FIR (Kaiser-windowed sinc, 32 table phases, linear interpolation between the outputs of the two nearest phases). Measured: passband within 0.5 dB to 20 kHz (the CIC's droop), aliases 79-99 dB down, DC exact. Like Paula, it is integer-only and defined by a C model (`host/resampler.c`) that the Verilog must match output for output (ADR-0005); the coefficient table is generated once by `tools/resampler_coefs.py` and committed, so no floating point is involved in a build.

The output instants are kept as an exact fraction of a colour clock (709379/153600 intermediate samples per output), so the Resampler makes exactly 48 000 samples per 3 546 895 frames, with no drift. The HDMI side takes exactly one sample every 525 pixel clocks. The 25.2 MHz pixel clock (the CEA-861 rate for 640x480 at 60 Hz) is made from the 50 MHz oscillator by a DCM (x21/25 = 42 MHz) and a PLL (x15 = 630 MHz, /25), so it is exactly 50 MHz x 63/125. Production and consumption therefore match on average exactly, and a 32-sample dual-clock FIFO only has to absorb Paula's catch-up jitter. 25.0 MHz (half the system clock) would have been simpler but is 0.7% outside the HDMI spec and some TVs refuse it.

## Considered options

- **Averaging the frames in each 1/48000 s**: tiny, but aliasing is audible on high Samples.
- **One-stage polyphase FIR at the colour-clock rate**: no CIC droop, but about 600 taps per output and a coefficient table too big for the block RAM we have.
- **Asynchronous sample-rate conversion (measuring the rates)**: needed only if the clocks were independent; ours aren't.

## Consequences

- Hardware HDMI audio equals the C model's output sample for sample after the FIFO has primed (`make resampler-test`, `make hdmi-chain-test`). Silence goes out until 16 samples are queued, and again whenever Paula stops and the FIFO runs dry.
- The Resampler needs frames at least 3 clocks apart; Paula's own state machine takes at least 4.
- Coefficients live in two RAMB16s and the FIFO and history in distributed RAM: no RAMB8s, which ISE 14.7's bitgen mishandles.
- HDMI sound is about 0.7 ms behind the jack (filter delay plus FIFO priming), which is inaudible.
- The board has a second and third clock domain (25.2 and 126 MHz); the FIFO's crossing is constrained as a false path.
- Channel 0 bit 3 in Data Islands is 0 on an island's first packet clock and 1 after (our reading of HDMI 1.3 figure 5-3; hdl-util/hdmi sends 1 throughout). The owner's display accepts it, picture and sound; if another refuses the signal, this is the first thing to try changing.
