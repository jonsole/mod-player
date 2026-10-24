/* MOD loader: accepts 4-channel 31-sample MOD files, rejects the rest. */
#include "check.h"
#include "modbuild.h"
#include "mod.h"

static modbuild_t mb;
static mod_song_t song;

static void reads_the_song_header_and_samples(void)
{
    mb_init(&mb, 2);
    uint8_t order[3] = {1, 0, 1};
    mb_positions(&mb, 3, order);
    mb_sample(&mb, 1, 4, 0, 48, 0, 1);
    mb_sample(&mb, 2, 6, 15, 64, 2, 3);
    uint32_t size = mb_finish(&mb);

    CHECK_EQ(mod_load(&song, mb.buf, size), MOD_OK);
    CHECK(strcmp(song.name, "test song") == 0);
    CHECK_EQ(song.length, 3);
    CHECK_EQ(song.order[0], 1);
    CHECK_EQ(song.num_patterns, 2);
    CHECK_EQ(song.pattern_offset, 1084);
    CHECK_EQ(song.sample[0].offset, 1084 + 2 * 1024);
    CHECK_EQ(song.sample[0].length, 4);
    CHECK_EQ(song.sample[0].volume, 48);
    CHECK_EQ(song.sample[0].replen, 1);
    CHECK_EQ(song.sample[1].offset, 1084 + 2 * 1024 + 8);
    CHECK_EQ(song.sample[1].finetune, 15);
    CHECK_EQ(song.sample[1].repeat, 2);
    CHECK_EQ(song.sample[1].replen, 3);
}

static void accepts_every_4_channel_tag(void)
{
    const char *tags[] = {"M.K.", "M!K!", "FLT4", "4CHN"};
    for (int i = 0; i < 4; i++) {
        mb_init(&mb, 1);
        memcpy(mb.buf + 1080, tags[i], 4);
        CHECK_EQ(mod_load(&song, mb.buf, mb_finish(&mb)), MOD_OK);
    }
}

static void rejects_files_with_more_channels(void)
{
    const char *tags[] = {"6CHN", "8CHN", "FLT8", "OKTA", "CD81", "16CH"};
    for (int i = 0; i < 6; i++) {
        mb_init(&mb, 1);
        memcpy(mb.buf + 1080, tags[i], 4);
        CHECK_EQ(mod_load(&song, mb.buf, mb_finish(&mb)), MOD_ERR_NOT_4_CHANNELS);
    }
}

static void rejects_files_without_a_mod_signature(void)
{
    mb_init(&mb, 1);
    memcpy(mb.buf + 1080, "\0\0\0\0", 4); /* e.g. a 15-sample Soundtracker file */
    CHECK_EQ(mod_load(&song, mb.buf, mb_finish(&mb)), MOD_ERR_NOT_A_MOD);
    CHECK_EQ(mod_load(&song, mb.buf, 600), MOD_ERR_NOT_A_MOD);
}

static void rejects_files_cut_short_in_the_patterns(void)
{
    mb_init(&mb, 2);
    CHECK_EQ(mod_load(&song, mb.buf, 1084 + 1024 + 10), MOD_ERR_TRUNCATED);
}

static void rejects_a_song_length_of_zero_or_over_128(void)
{
    mb_init(&mb, 1);
    mb.buf[950] = 0;
    CHECK_EQ(mod_load(&song, mb.buf, mb_finish(&mb)), MOD_ERR_NOT_A_MOD);
    mb.buf[950] = 129;
    CHECK_EQ(mod_load(&song, mb.buf, mb_finish(&mb)), MOD_ERR_NOT_A_MOD);
}

static void every_error_has_a_message(void)
{
    for (int e = MOD_OK; e <= MOD_ERR_TRUNCATED; e++)
        CHECK(mod_error_text((mod_error_t)e)[0] != '\0');
}

/* As ProTracker: a non-looping sample's first word is cleared, so the
 * one-word loop Paula falls back to is silent. */
static void clears_the_first_word_of_non_looping_samples(void)
{
    mb_init(&mb, 1);
    mb_sample(&mb, 1, 4, 0, 64, 0, 1); /* no loop */
    mb_sample(&mb, 2, 4, 0, 64, 1, 2); /* loops */
    uint32_t size = mb_finish(&mb);
    CHECK_EQ(mod_load(&song, mb.buf, size), MOD_OK);
    uint32_t s1 = song.sample[0].offset, s2 = song.sample[1].offset;
    CHECK_EQ(mb.buf[s1], 0);
    CHECK_EQ(mb.buf[s1 + 1], 0);
    CHECK_EQ(mb.buf[s1 + 2], 3);
    CHECK_EQ(mb.buf[s2], 9);
}

static void caps_volume_at_64_and_loops_at_one_word_minimum(void)
{
    mb_init(&mb, 1);
    mb_sample(&mb, 1, 4, 0, 99, 0, 0);
    CHECK_EQ(mod_load(&song, mb.buf, mb_finish(&mb)), MOD_OK);
    CHECK_EQ(song.sample[0].volume, 64);
    CHECK_EQ(song.sample[0].replen, 1);
}

static void samples_cut_short_by_the_end_of_file_are_shortened(void)
{
    mb_init(&mb, 1);
    mb_sample(&mb, 1, 4, 0, 64, 0, 1);
    mb_sample(&mb, 2, 100, 0, 64, 0, 1);
    uint32_t size = mb_finish(&mb);
    CHECK_EQ(mod_load(&song, mb.buf, size - 100), MOD_OK); /* 50 words missing */
    CHECK_EQ(song.sample[1].length, 50);
}

int main(void)
{
    RUN(reads_the_song_header_and_samples);
    RUN(accepts_every_4_channel_tag);
    RUN(rejects_files_with_more_channels);
    RUN(rejects_files_without_a_mod_signature);
    RUN(rejects_files_cut_short_in_the_patterns);
    RUN(rejects_a_song_length_of_zero_or_over_128);
    RUN(every_error_has_a_message);
    RUN(clears_the_first_word_of_non_looping_samples);
    RUN(caps_volume_at_64_and_loops_at_one_word_minimum);
    RUN(samples_cut_short_by_the_end_of_file_are_shortened);
    CHECK_DONE();
}
