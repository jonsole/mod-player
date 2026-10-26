#include "status.h"
#include "console.h"
#include "hw.h"
#include "player.h"
#include "serial.h"
#include "version.h"

/* The layout (53 x 20):
 *   row 0   title bar: "mod-player <version>", "playing" / "paused" / "stopped"
 *   row 1   the Song's name
 *   row 2   Pos 003/029  Pat 007  Row 12  Spd 06  BPM 125
 *   row 3   a rule
 *   4..11   the four Voice scopes, drawn by the hardware over these rows
 *           (hw/rtl/scopes.v); the text here is just their numbers
 *   12..19  the spectrum analyser of the mix, drawn by the hardware
 *           (hw/rtl/spectrum.v) */
#define TITLE   ATTR(WHITE, BLUE)
#define NAME    ATTR(YELLOW, BLACK)
#define LABEL   ATTR(DARK_GREY, BLACK)
#define VALUE   ATTR(WHITE, BLACK)
#define TEXT    ATTR_NORMAL
#define POS_ROW 2

static uint16_t want[CONSOLE_ROWS][CONSOLE_COLS];   /* attribute << 8 | character */
static uint16_t shown[CONSOLE_ROWS][CONSOLE_COLS];  /* what the console has */
static int active;
static uint32_t drawn_load;
static player_position_t drawn_pos;
static int drawn_pos_valid, drawn_state;

static void put(int row, int col, uint8_t ch, uint8_t attr)
{
    if (row >= 0 && row < CONSOLE_ROWS && col >= 0 && col < CONSOLE_COLS)
        want[row][col] = (uint16_t)(attr << 8 | ch);
}

/* Writes s in a field of `width` cells, padded with spaces. */
static int text(int row, int col, const char *s, int width, uint8_t attr)
{
    for (int i = 0; i < width; i++)
        put(row, col + i, (uint8_t)(*s ? *s++ : ' '), attr);
    return col + width;
}

static int number(int row, int col, unsigned v, int digits, uint8_t attr)
{
    for (int i = digits - 1; i >= 0; i--, v /= 10)
        put(row, col + i, (uint8_t)('0' + v % 10), attr);
    return col + digits;
}

static void draw_state(int state)
{
    static const char *const names[] = {"stopped", "playing", "paused"};
    text(0, CONSOLE_COLS - 9, names[state], 9, TITLE);
}

static void draw_position(const player_position_t *p, int valid)
{
    int c = text(POS_ROW, 1, "Pos ", 4, LABEL);
    if (valid) {
        c = number(POS_ROW, c, p->pos, 3, VALUE);
        c = text(POS_ROW, c, "/", 1, LABEL);
        c = number(POS_ROW, c, player_song()->length, 3, VALUE);
        c = text(POS_ROW, c, "  Pat ", 6, LABEL);
        c = number(POS_ROW, c, p->pattern, 3, VALUE);
        c = text(POS_ROW, c, "  Row ", 6, LABEL);
        c = number(POS_ROW, c, p->row, 2, VALUE);
        c = text(POS_ROW, c, "  Spd ", 6, LABEL);
        c = number(POS_ROW, c, p->speed, 2, VALUE);
        c = text(POS_ROW, c, "  BPM ", 6, LABEL);
        number(POS_ROW, c, p->bpm, 3, VALUE);
    } else {
        text(POS_ROW, c, "-", CONSOLE_COLS - 1 - c, VALUE);
    }
}

static void draw_song(const mod_song_t *s)
{
    for (int r = 0; r < CONSOLE_ROWS; r++)
        text(r, 0, "", CONSOLE_COLS, TEXT);
    text(0, 0, " mod-player " VERSION, CONSOLE_COLS, TITLE);
    text(1, 1, s->name, 20, NAME);
    for (int c = 1; c < CONSOLE_COLS - 1; c++)
        put(3, c, 0xC4, LABEL);                         /* the rule: a line */
    static const int scope_col[4] = {0, 14, 27, 40};      /* where each scope starts */
    for (int v = 0; v < 4; v++)
        put(4, scope_col[v], (uint8_t)('1' + v), LABEL);
}

/* Copies changed cells to the console, but only in the blanking, and only
 * while it lasts; the rest waits for the next one. It also stops for a
 * byte from the PC: the serial port holds only one, and the bytes of a
 * command come back to back. */
static void flush(void)
{
    if (!(CONSOLE_STATUS & 0x40000000u))               /* not in the blanking */
        return;
    for (int r = 0; r < CONSOLE_ROWS; r++) {
        for (int c = 0; c < CONSOLE_COLS; c++)
            if (want[r][c] != shown[r][c]) {
                if (serial_avail())
                    return;
                console_put(r, c, (uint8_t)want[r][c], (uint8_t)(want[r][c] >> 8));
                shown[r][c] = want[r][c];
            }
        if (!(CONSOLE_STATUS & 0x40000000u))
            return;
    }
}

void status_update(void)
{
    const mod_song_t *s = player_song();
    if (!console_present() || !s)
        return;
    if (!active || drawn_load != player_load_count()) {
        draw_song(s);
        for (int r = 0; r < CONSOLE_ROWS; r++)
            for (int c = 0; c < CONSOLE_COLS; c++)
                shown[r][c] = 0xFFFFu;                  /* redraw every cell */
        active = 1;
        console_control(CONSOLE_SCOPES | CONSOLE_SPECTRUM);
        drawn_load = player_load_count();
        drawn_pos_valid = -1;
        drawn_state = -1;
    }
    player_position_t p;
    int valid = player_position(&p);
    if (valid != drawn_pos_valid || p.pos != drawn_pos.pos || p.row != drawn_pos.row ||
        p.pattern != drawn_pos.pattern || p.speed != drawn_pos.speed || p.bpm != drawn_pos.bpm) {
        draw_position(&p, valid);
        drawn_pos = p;
        drawn_pos_valid = valid;
    }
    int state = !player_playing() ? 0 : player_paused() ? 2 : 1;
    if (state != drawn_state) {
        draw_state(state);
        drawn_state = state;
    }
    flush();
}
