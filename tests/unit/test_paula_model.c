/* Paula C model, tested at its interface: register writes in, samples out. */
#include <string.h>
#include "check.h"
#include "paula_model.h"

static paula_model_t paula;

static void start_voice(int v, uint32_t ptr, uint32_t len, uint32_t per, uint32_t vol)
{
    paula_model_write(&paula, PAULA_VOICE(v) + PAULA_PTR, ptr);
    paula_model_write(&paula, PAULA_VOICE(v) + PAULA_LEN, len);
    paula_model_write(&paula, PAULA_VOICE(v) + PAULA_PER, per);
    paula_model_write(&paula, PAULA_VOICE(v) + PAULA_VOL, vol);
    paula_model_write(&paula, PAULA_DMACON, PAULA_DMACON_SET | (1u << v));
}

/* Steps n colour clocks and checks every one of them gives (left, right). */
static void expect_frames(int n, int32_t left, int32_t right)
{
    for (int i = 0; i < n; i++) {
        paula_frame_t f = paula_model_step(&paula);
        CHECK_EQ(f.left, left);
        CHECK_EQ(f.right, right);
        if (f.left != left || f.right != right) {
            printf("  ...at colour clock %d of %d\n", i, n);
            return;
        }
    }
}

static const int8_t ramp[8] = {10, -20, 30, 40, 50, 60, 70, 80};

static void a_started_voice_plays_each_sample_for_one_period(void)
{
    paula_model_reset(&paula, (const uint8_t *)ramp, sizeof ramp);
    start_voice(0, 0, 4, 120, 64);
    expect_frames(120, 10 * 64, 0);
    expect_frames(120, -20 * 64, 0);
    expect_frames(120, 30 * 64, 0);
}

static void voices_1_and_4_play_left_2_and_3_play_right(void)
{
    paula_model_reset(&paula, (const uint8_t *)ramp, sizeof ramp);
    start_voice(0, 0, 4, 200, 1);  /* left:  10 */
    start_voice(1, 2, 3, 200, 2);  /* right: 30 * 2 */
    start_voice(2, 4, 2, 200, 1);  /* right: 50 */
    start_voice(3, 6, 1, 200, 1);  /* left:  70 */
    expect_frames(1, 10 + 70, 60 + 50);
}

static void silent_until_started_and_after_stopped(void)
{
    paula_model_reset(&paula, (const uint8_t *)ramp, sizeof ramp);
    expect_frames(10, 0, 0);
    start_voice(0, 0, 4, 120, 64);
    expect_frames(1, 640, 0);
    paula_model_write(&paula, PAULA_DMACON, 1u);
    expect_frames(10, 0, 0);
}

/* Amiga-style looping: PTR/LEN written after the start become the loop. */
static void at_the_end_playback_continues_from_the_latched_loop(void)
{
    paula_model_reset(&paula, (const uint8_t *)ramp, sizeof ramp);
    start_voice(0, 0, 2, 113, 1);                     /* bytes 0..3 once */
    paula_model_write(&paula, PAULA_VOICE(0) + PAULA_PTR, 6);
    paula_model_write(&paula, PAULA_VOICE(0) + PAULA_LEN, 1); /* then loop bytes 6..7 */
    int8_t expected[] = {10, -20, 30, 40, 70, 80, 70, 80, 70};
    for (unsigned i = 0; i < sizeof expected; i++)
        expect_frames(113, expected[i], 0);
}

/* A sample with no loop: ProTracker loops its first word, which the loader
 * has cleared, so the voice falls silent. */
static void a_one_word_loop_of_zeros_is_silence(void)
{
    static const int8_t smp[6] = {0, 0, 5, 6, 7, 8};
    paula_model_reset(&paula, (const uint8_t *)smp, sizeof smp);
    start_voice(0, 0, 3, 113, 64);
    paula_model_write(&paula, PAULA_VOICE(0) + PAULA_LEN, 1);
    int8_t expected[] = {0, 0, 5, 6, 7, 8, 0, 0, 0, 0};
    for (unsigned i = 0; i < sizeof expected; i++)
        expect_frames(113, expected[i] * 64, 0);
}

static void a_new_period_takes_effect_at_the_next_sample_step(void)
{
    paula_model_reset(&paula, (const uint8_t *)ramp, sizeof ramp);
    start_voice(0, 0, 4, 200, 1);
    expect_frames(50, 10, 0);
    paula_model_write(&paula, PAULA_VOICE(0) + PAULA_PER, 300);
    expect_frames(150, 10, 0);  /* current sample keeps its old period */
    expect_frames(300, -20, 0);
    expect_frames(300, 30, 0);
}

static void a_new_volume_takes_effect_at_the_next_sample_step(void)
{
    paula_model_reset(&paula, (const uint8_t *)ramp, sizeof ramp);
    start_voice(0, 0, 4, 200, 1);
    expect_frames(50, 10, 0);
    paula_model_write(&paula, PAULA_VOICE(0) + PAULA_VOL, 2);
    expect_frames(150, 10, 0);
    expect_frames(200, -40, 0);
}

static void periods_below_113_act_as_113_and_zero_as_65536(void)
{
    paula_model_reset(&paula, (const uint8_t *)ramp, sizeof ramp);
    start_voice(0, 0, 4, 1, 1);
    expect_frames(113, 10, 0);
    expect_frames(1, -20, 0);
    start_voice(1, 0, 4, 0, 1);
    paula_model_step(&paula); /* right channel only from here */
    for (int i = 1; i < 65536; i++)
        CHECK_EQ(paula_model_step(&paula).right, 10);
    CHECK_EQ(paula_model_step(&paula).right, -20);
}

static void volumes_above_64_act_as_64(void)
{
    paula_model_reset(&paula, (const uint8_t *)ramp, sizeof ramp);
    start_voice(0, 0, 4, 200, 100);  /* bit 6 set -> 64 */
    expect_frames(1, 640, 0);
    start_voice(0, 0, 4, 200, 0x85); /* bit 7 ignored -> 5 */
    expect_frames(1, 50, 0);
}

static void a_length_of_zero_plays_65536_words(void)
{
    static uint8_t big[131074];
    memset(big, 0, sizeof big);
    big[0] = 1;
    big[131072] = 2; /* first byte past 65536 words: must not be reached */
    paula_model_reset(&paula, big, sizeof big);
    start_voice(0, 0, 0, 113, 1);
    expect_frames(113, 1, 0);
    for (long i = 0; i < 131071L * 113; i++) /* bytes 1..131071 */
        paula_model_step(&paula);
    expect_frames(1, 1, 0); /* wrapped back to byte 0, not byte 131072 */
}

int main(void)
{
    RUN(at_the_end_playback_continues_from_the_latched_loop);
    RUN(a_one_word_loop_of_zeros_is_silence);
    RUN(a_new_period_takes_effect_at_the_next_sample_step);
    RUN(a_new_volume_takes_effect_at_the_next_sample_step);
    RUN(periods_below_113_act_as_113_and_zero_as_65536);
    RUN(volumes_above_64_act_as_64);
    RUN(a_length_of_zero_plays_65536_words);
    RUN(a_started_voice_plays_each_sample_for_one_period);
    RUN(voices_1_and_4_play_left_2_and_3_play_right);
    RUN(silent_until_started_and_after_stopped);
    CHECK_DONE();
}
