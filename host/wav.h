/* Writes 16-bit stereo PCM WAV files. */
#ifndef WAV_H
#define WAV_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    uint32_t rate;
    uint32_t frames;
} wav_writer_t;

/* Returns 0 on success. */
int wav_open(wav_writer_t *w, const char *path, uint32_t rate);
void wav_frame(wav_writer_t *w, int16_t left, int16_t right);
/* Fills in the header's sizes and closes the file. Returns 0 on success. */
int wav_close(wav_writer_t *w);

#endif
