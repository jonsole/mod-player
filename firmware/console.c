#include "console.h"
#include "hw.h"

#define CELL(n)          REG32(CONSOLE_BASE + 4u * (uint32_t)(n))
#define STATUS_PRESENT   0x80000000u
#define STATUS_BLANKING  0x40000000u
#define STATUS_FRAMES    0x00FFFFFFu

int console_present(void)
{
    return (CONSOLE_STATUS & STATUS_PRESENT) != 0;
}

uint32_t console_frames(void)
{
    return CONSOLE_STATUS & STATUS_FRAMES;
}

void console_wait_blanking(void)
{
    if (!console_present())
        return;
    while (CONSOLE_STATUS & STATUS_BLANKING)
        ;
    while (!(CONSOLE_STATUS & STATUS_BLANKING))
        ;
}

void console_put(int row, int col, uint8_t ch, uint8_t attr)
{
    if (row < 0 || row >= CONSOLE_ROWS || col < 0 || col >= CONSOLE_COLS)
        return;
    CELL(row * CONSOLE_COLS + col) = (uint32_t)attr << 8 | ch;
}

int console_print(int row, int col, const char *s, uint8_t attr)
{
    while (*s && col < CONSOLE_COLS)
        console_put(row, col++, (uint8_t)*s++, attr);
    return col;
}

void console_clear_row(int row, int col, uint8_t attr)
{
    while (col < CONSOLE_COLS)
        console_put(row, col++, ' ', attr);
}

void console_control(uint32_t bits)
{
    if (console_present())
        CONSOLE_STATUS = bits;
}

void console_clear(uint8_t attr)
{
    for (int row = 0; row < CONSOLE_ROWS; row++)
        console_clear_row(row, 0, attr);
}
