/* MOD loader: reads a 4-channel, 31-sample MOD file that is already in
 * memory (ADR-0003). The file's bytes become the Song's memory: Patterns and
 * Samples are used in place, so nothing is copied.
 */
#ifndef MOD_H
#define MOD_H

#include <stdint.h>

#define MOD_SAMPLES 31
#define MOD_ROWS 64
#define MOD_CHANNELS 4
#define MOD_PATTERN_BYTES (MOD_ROWS * MOD_CHANNELS * 4)

typedef struct {
    char name[23];
    uint32_t offset;   /* where its data starts in Song memory */
    uint16_t length;   /* in words */
    uint16_t repeat;   /* loop start, in words */
    uint16_t replen;   /* loop length in words; 1 means "no loop" */
    uint8_t finetune;  /* 0..15 (8..15 are -8..-1) */
    uint8_t volume;    /* 0..64 */
} mod_sample_t;

typedef struct {
    char name[21];
    uint8_t length;           /* number of Positions, 1..128 */
    uint8_t order[128];       /* Pattern number for each Position */
    uint8_t num_patterns;
    uint32_t pattern_offset;  /* where Pattern 0 starts in Song memory */
    mod_sample_t sample[MOD_SAMPLES];
} mod_song_t;

typedef enum {
    MOD_OK,
    MOD_ERR_NOT_A_MOD,
    MOD_ERR_NOT_4_CHANNELS,
    MOD_ERR_TRUNCATED,
} mod_error_t;

/* Parses data[0..size) into song. On success the data is also modified as
 * ProTracker does (first word of non-looping Samples cleared). */
mod_error_t mod_load(mod_song_t *song, uint8_t *data, uint32_t size);

const char *mod_error_text(mod_error_t err);

#endif
