/* The HDMI text console (hw/rtl/text_console.v, ADR-0012): 53 x 20
 * characters of 12 x 24 pixels in code page 437, each with an attribute:
 * foreground colour in the low nibble, background in the high one. Only the HDMI build has one;
 * without it console_present() is 0 and the rest do nothing harmful. */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

#define CONSOLE_COLS 53
#define CONSOLE_ROWS 20

enum {
    BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHT_GREY,
    DARK_GREY, LIGHT_BLUE, LIGHT_GREEN, LIGHT_CYAN, LIGHT_RED, LIGHT_MAGENTA, YELLOW, WHITE
};
#define ATTR(fg, bg) ((uint8_t)((bg) << 4 | (fg)))
#define ATTR_NORMAL ATTR(LIGHT_GREY, BLACK)

int console_present(void);
/* Frames shown since power-on. */
uint32_t console_frames(void);
/* Waits for the start of the next vertical blanking (1.4 ms in which
 * writes can't be seen half done). Returns at once without a console. */
void console_wait_blanking(void);
void console_put(int row, int col, uint8_t ch, uint8_t attr);
/* Writes s from (row, col), clipped at the end of the row; returns the
 * column after it. */
int console_print(int row, int col, const char *s, uint8_t attr);
/* Fills a row from col to the end with spaces. */
void console_clear_row(int row, int col, uint8_t attr);
void console_clear(uint8_t attr);

/* The control register: what the hardware draws over the text. */
#define CONSOLE_SCOPES   0x1u    /* the four Voice scopes, rows 4-11 (hw/rtl/scopes.v) */
#define CONSOLE_SPECTRUM 0x2u    /* the spectrum analyser, rows 12-19 (hw/rtl/spectrum.v) */
void console_control(uint32_t bits);

#endif
