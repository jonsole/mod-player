// The spectrum analyser's picture (hw/rtl/spectrum.v, ADR-0016), for the
// testbenches: 24 bars, each a stack of 25 LEDs of 23 x 5 pixels, 26 apart
// across from x = 8 and 7 apart up from y = 479. Given the bars' heights and
// peaks as the display copied them, the colour of pixel (x, y): the peak LED
// white, the bar's LEDs a little dimmer, unlit ones faint grey, or 0 where
// the analyser draws nothing (the gaps).
#ifndef SPECTRUM_LEDS_H
#define SPECTRUM_LEDS_H

#include <cstdint>

static const uint32_t SPECTRUM_PEAK = 0xF4F4F4, SPECTRUM_LIT = 0xB0B0B0, SPECTRUM_UNLIT = 0x121212;

static uint32_t spectrum_led_pixel(int x, int y, const uint8_t *h, const uint8_t *p)
{
    if (x < 8 || x >= 632 || y < 288 || y >= 480) return 0;
    int bx = x - 8, bar = bx / 26, pos = 479 - y, led = pos / 7;
    if (bx % 26 >= 23 || pos % 7 >= 5 || led >= 25) return 0;          // the gaps; 25 LEDs
    if (led > 0 && p[bar] / 7 == led) return SPECTRUM_PEAK;            // the peak
    if (h[bar] >= 7 * (led + 1)) return SPECTRUM_LIT;                   // part of the bar
    return SPECTRUM_UNLIT;
}

#endif
