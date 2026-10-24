/* Replayer, tested at the Paula port: Song in, register writes out.
 * Expected writes are worked out by hand from ProTracker 2.3's logic;
 * the pt2-clone oracle takes over from these in ticket #3. */
#include "check.h"
#include "modbuild.h"
#include "mod.h"
#include "replayer.h"

#define NULL_ADDR 0x7F0000u
#define S1 (MB_HEADER + 1024) /* sample 1 data, one-Pattern songs */

typedef struct { uint32_t reg, value; } write_t;
static write_t log_[4096];
static int nlog;

static void record(void *ctx, uint32_t reg, uint32_t value)
{
    (void)ctx;
    if (nlog < 4096)
        log_[nlog++] = (write_t){reg, value};
}

static modbuild_t mb;
static mod_song_t song;
static replayer_t rp;

static void start(void)
{
    uint32_t size = mb_finish(&mb);
    CHECK_EQ(mod_load(&song, mb.buf, size), MOD_OK);
    nlog = 0;
    replayer_start(&rp, &song, mb.buf, 0, NULL_ADDR, (paula_port_t){record, 0});
}

/* Checks the log holds exactly these writes, then clears it. */
#define V(v, r) (PAULA_VOICE(v) + PAULA_##r)
static void expect_writes(const write_t *w, int n)
{
    CHECK_EQ(nlog, n);
    for (int i = 0; i < n && i < nlog; i++) {
        CHECK_EQ(log_[i].reg, w[i].reg);
        CHECK_EQ(log_[i].value, w[i].value);
        if (log_[i].reg != w[i].reg || log_[i].value != w[i].value) {
            printf("  ...at write %d\n", i);
            break;
        }
    }
    nlog = 0;
}

static void one_note_song(void)
{
    mb_init(&mb, 1);
    mb_sample(&mb, 1, 4, 0, 48, 0, 1);
    mb_note(&mb, 0, 0, 0, 428, 1, 0, 0);
}

static void starting_silences_every_voice(void)
{
    one_note_song();
    start();
    write_t w[] = {{PAULA_DMACON, 0xF}, {V(0, VOL), 0}, {V(1, VOL), 0}, {V(2, VOL), 0}, {V(3, VOL), 0}};
    expect_writes(w, 5);
}

static void the_first_tick_triggers_the_first_rows_notes(void)
{
    one_note_song();
    start();
    nlog = 0;
    replayer_tick(&rp);
    write_t w[] = {
        /* channel 1: note C-2 with Sample 1 */
        {V(0, PER), 0}, {PAULA_DMACON, 0x1}, {V(0, LEN), 4}, {V(0, PTR), S1}, {V(0, PER), 428},
        {V(0, PER), 428}, {V(0, VOL), 48},
        /* channels 2-4: empty */
        {V(1, PER), 0}, {V(1, PER), 0}, {V(1, VOL), 0},
        {V(2, PER), 0}, {V(2, PER), 0}, {V(2, VOL), 0},
        {V(3, PER), 0}, {V(3, PER), 0}, {V(3, VOL), 0},
        /* start triggered voices, then set every voice's loop */
        {PAULA_DMACON, 0x8001},
        {V(0, PTR), S1}, {V(0, LEN), 1},
        {V(1, PTR), NULL_ADDR}, {V(1, LEN), 0},
        {V(2, PTR), NULL_ADDR}, {V(2, LEN), 0},
        {V(3, PTR), NULL_ADDR}, {V(3, LEN), 0},
    };
    expect_writes(w, (int)(sizeof w / sizeof w[0]));
}

static void ticks_between_rows_only_refresh_volumes(void)
{
    one_note_song();
    start();
    replayer_tick(&rp);
    nlog = 0;
    for (int t = 2; t <= 6; t++) {
        replayer_tick(&rp);
        write_t w[] = {{V(0, VOL), 48}, {V(1, VOL), 0}, {V(2, VOL), 0}, {V(3, VOL), 0}};
        expect_writes(w, 4);
    }
}

static void an_empty_row_keeps_the_note_playing(void)
{
    one_note_song();
    start();
    for (int t = 1; t <= 6; t++)
        replayer_tick(&rp);
    nlog = 0;
    replayer_tick(&rp); /* tick 7: row 1, empty */
    write_t w[] = {
        {V(0, PER), 428}, {V(0, VOL), 48},
        {V(1, PER), 0}, {V(1, PER), 0}, {V(1, VOL), 0},
        {V(2, PER), 0}, {V(2, PER), 0}, {V(2, VOL), 0},
        {V(3, PER), 0}, {V(3, PER), 0}, {V(3, VOL), 0},
        {PAULA_DMACON, 0x8000},
        {V(0, PTR), S1}, {V(0, LEN), 1},
        {V(1, PTR), NULL_ADDR}, {V(1, LEN), 0},
        {V(2, PTR), NULL_ADDR}, {V(2, LEN), 0},
        {V(3, PTR), NULL_ADDR}, {V(3, LEN), 0},
    };
    expect_writes(w, (int)(sizeof w / sizeof w[0]));
}

/* A Sample with a loop: Paula plays repeat+replen words first, then loops. */
static void a_looping_sample_sets_its_loop_after_starting(void)
{
    mb_init(&mb, 1);
    mb_sample(&mb, 1, 8, 0, 64, 2, 4);
    mb_note(&mb, 0, 0, 0, 214, 1, 0, 0);
    start();
    nlog = 0;
    replayer_tick(&rp);
    CHECK_EQ(log_[2].reg, V(0, LEN));
    CHECK_EQ(log_[2].value, 2 + 4);
    CHECK_EQ(log_[17].reg, V(0, PTR));
    CHECK_EQ(log_[17].value, S1 + 4);
    CHECK_EQ(log_[18].value, 4);
}

/* Returns the tick numbers (1-based) on which a row was read. */
static int row_ticks(int ticks, int *out, int max)
{
    int n = 0;
    for (int t = 1; t <= ticks; t++) {
        nlog = 0;
        replayer_tick(&rp);
        for (int i = 0; i < nlog; i++)
            if (log_[i].reg == PAULA_DMACON && (log_[i].value & PAULA_DMACON_SET) && n < max)
                out[n++] = t;
    }
    return n;
}

static void speed_effect_sets_ticks_per_row(void)
{
    one_note_song();
    mb_note(&mb, 0, 0, 1, 0, 0, 0xF, 3);
    start();
    int t[4];
    CHECK_EQ(row_ticks(10, t, 4), 4);
    CHECK_EQ(t[0], 1);
    CHECK_EQ(t[1], 4);
    CHECK_EQ(t[2], 7);
    CHECK_EQ(t[3], 10);
}

/* Default tempo 125 BPM: CIA timer 1773447/125 = 14187; the CIA fires on
 * underflow, so a tick is 14188 CIA cycles of 5 colour clocks each. */
static void each_tick_lasts_one_cia_period_at_125_bpm(void)
{
    one_note_song();
    start();
    CHECK_EQ(replayer_tick(&rp), 70940);
    CHECK_EQ(replayer_tick(&rp), 70940);
}

/* The last period written to voice 1 this tick (an empty row first
 * rewrites the old period, so the first write is not the new note). */
static uint32_t last_period_written(void)
{
    uint32_t per = 0;
    for (int i = 0; i < nlog; i++)
        if (log_[i].reg == V(0, PER))
            per = log_[i].value;
    return per;
}

static void after_the_last_row_play_moves_to_the_next_position_and_wraps(void)
{
    mb_init(&mb, 2);
    uint8_t order[2] = {1, 0}; /* Pattern 1 first, so a missing wrap shows */
    mb_positions(&mb, 2, order);
    mb_sample(&mb, 1, 4, 0, 64, 0, 1);
    mb_note(&mb, 0, 0, 0, 428, 1, 0, 0);
    mb_note(&mb, 1, 0, 0, 214, 1, 0, 0);
    start();
    int row = 0;
    uint32_t seen[3] = {0};
    for (int t = 1; t <= 6 * 129; t++) {
        nlog = 0;
        replayer_tick(&rp);
        if ((t - 1) % 6 == 0) {
            if (row == 0) seen[0] = last_period_written();
            if (row == 64) seen[1] = last_period_written();
            if (row == 128) seen[2] = last_period_written();
            row++;
        }
    }
    CHECK_EQ(seen[0], 214);
    CHECK_EQ(seen[1], 428);
    CHECK_EQ(seen[2], 214);
}

/* Ticks until `ended` is first set (0 if not within max). */
static int ticks_until_end(int max)
{
    for (int t = 1; t <= max; t++) {
        replayer_tick(&rp);
        if (rp.ended)
            return t;
    }
    return 0;
}

static void a_one_pattern_song_ends_after_64_rows(void)
{
    one_note_song();
    start();
    CHECK_EQ(ticks_until_end(2000), 64 * 6);
}

static void a_song_that_jumps_back_ends_when_a_row_repeats(void)
{
    mb_init(&mb, 2);
    uint8_t order[3] = {0, 1, 0};
    mb_positions(&mb, 3, order);
    mb_note(&mb, 1, 63, 0, 0, 0, 0xB, 0x00); /* Position 1 jumps back to Position 0 */
    start();
    CHECK_EQ(ticks_until_end(2000), 2 * 64 * 6);
}

static void a_pattern_loop_is_not_the_end(void)
{
    one_note_song();
    mb_note(&mb, 0, 1, 1, 0, 0, 0xE, 0x60);
    mb_note(&mb, 0, 3, 1, 0, 0, 0xE, 0x61); /* rows 1-3 play twice */
    start();
    CHECK_EQ(ticks_until_end(2000), (64 + 3) * 6);
}

static void the_end_is_reported_once_per_play_through_and_play_continues(void)
{
    one_note_song();
    start();
    CHECK_EQ(ticks_until_end(2000), 64 * 6);
    rp.ended = 0; /* a looping player may clear it to hear about the next pass */
    CHECK_EQ(ticks_until_end(2000), 64 * 6);
}

static void f00_stops_the_song(void)
{
    one_note_song();
    mb_note(&mb, 0, 2, 1, 0, 0, 0xF, 0x00);
    start();
    for (int t = 1; t <= 13; t++)
        replayer_tick(&rp);
    CHECK_EQ(rp.stopped, 1);
    nlog = 0;
    replayer_tick(&rp);
    CHECK_EQ(nlog, 0);
}

int main(void)
{
    RUN(a_one_pattern_song_ends_after_64_rows);
    RUN(a_song_that_jumps_back_ends_when_a_row_repeats);
    RUN(a_pattern_loop_is_not_the_end);
    RUN(the_end_is_reported_once_per_play_through_and_play_continues);
    RUN(f00_stops_the_song);
    RUN(starting_silences_every_voice);
    RUN(the_first_tick_triggers_the_first_rows_notes);
    RUN(ticks_between_rows_only_refresh_volumes);
    RUN(an_empty_row_keeps_the_note_playing);
    RUN(a_looping_sample_sets_its_loop_after_starting);
    RUN(speed_effect_sets_ticks_per_row);
    RUN(each_tick_lasts_one_cia_period_at_125_bpm);
    RUN(after_the_last_row_play_moves_to_the_next_position_and_wraps);
    CHECK_DONE();
}
