# The spectrum analyser is a hardware FFT of the Resampler's output, matched to a C model

*Its look changed with ADR-0016: 24 bars of white LEDs, not 48 coloured bars; the analysis is the same.*

The status screen's bottom half (rows 12-19) is a spectrum analyser of the mix: 48 bars, 12 pixels wide and 13 apart from x = 8, standing on the bottom of the screen, coloured green to yellow to red by height, each with a peak marker that holds for half a second and then falls (the owner's choice of look). It is computed by hardware (`hw/rtl/spectrum.v`) and defined, like Paula and the Resampler, by an integer C model (`host/spectrum.c`) that the Verilog must match bar for bar (ADR-0005).

Its input is the HDMI Resampler's 48 kHz output (ADR-0010), left and right mixed, which is already low-pass filtered: decimating Paula's 3.5 MHz frames directly would fold ultrasonic images into the bars. Once a video frame the last 1024 samples are windowed (Hann), transformed by a radix-2 FFT that halves at every stage (so 18-bit values never overflow; the result is the DFT / 1024), and each bar takes its loudest bin, in eighths of an octave of power (0.38 dB a pixel, a 68 dB range over 180 pixels). Bars are spaced logarithmically from 50 Hz to 20 kHz, except that every bar gets at least one bin of its own: at 46.9 Hz a bin, the lowest 26 bars step up a bin at a time to about 1.2 kHz. The tables (window, a quarter-wave sine for the twiddle factors, the bars' bins) are generated once by `tools/spectrum_tables.py` and committed.

An analysis takes about 12 000 system clocks (0.24 ms): loading 1024 samples, 10 stages of 512 butterflies at two clocks each (four DSP multipliers), then one clock a bin for the bars. It starts at the start of the vertical blanking, the same moment the display copies the bars it will draw, and changes them only 0.24 ms later, so the display never sees half an update.

## Considered options

- **The FFT in firmware**: no new hardware, but a few milliseconds of CPU a frame, and the serial port holds only one byte, so a command arriving mid-FFT would lose bytes.
- **Decimating Paula's frames directly**: no filter needed, but aliasing; the Resampler already does the filtering.
- **A 2048-point FFT** (23 Hz bins, a better bass end): eight block RAMs, which is all that is left.
- **A bank of band-pass filters**: per-band cost grows with the bar count, and they are harder to make exact.

## Consequences

- `make spectrum-test` runs the Verilog against the C model over tones, a sweep, noise, clipping, silence and a Song, bar for bar, and checks every pixel it draws; `test_spectrum` pins the model (a tone lights its own bar, 40 dB is 106 pixels, the falls and the peak hold, the FFT against a plain DFT).
- Block RAM: the sample ring, the two FFT RAMs and the window (4 RAMB16s; the small tables are in logic).
- The firmware turns it on with bit 1 of the console's control register, with the scopes, while the status screen is up.
