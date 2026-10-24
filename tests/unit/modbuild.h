/* Builds small MOD files in memory for tests. */
#ifndef MODBUILD_H
#define MODBUILD_H

#include <stdint.h>
#include <string.h>

#define MB_HEADER 1084

typedef struct {
    uint8_t buf[MB_HEADER + 4 * 1024 + 65536];
    int num_patterns;
    uint32_t sample_bytes;
} modbuild_t;

static inline void mb_put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* An empty Song: one Position playing Pattern 0, tag M.K. */
static inline void mb_init(modbuild_t *m, int num_patterns)
{
    memset(m, 0, sizeof *m);
    memcpy(m->buf, "test song", 9);
    m->buf[950] = 1;   /* song length */
    m->buf[951] = 127;
    memcpy(m->buf + 1080, "M.K.", 4);
    m->num_patterns = num_patterns;
    if (num_patterns > 1)
        m->buf[952 + 1] = (uint8_t)(num_patterns - 1); /* make the count visible */
}

static inline void mb_positions(modbuild_t *m, int n, const uint8_t *patterns)
{
    m->buf[950] = (uint8_t)n;
    memcpy(m->buf + 952, patterns, (size_t)n);
}

/* Sample n (1..31): lengths in words. */
static inline void mb_sample(modbuild_t *m, int n, unsigned len_words, int finetune, int volume,
                      unsigned repeat_words, unsigned replen_words)
{
    uint8_t *h = m->buf + 20 + (n - 1) * 30;
    memcpy(h, "sample", 6);
    mb_put16(h + 22, len_words);
    h[24] = (uint8_t)(finetune & 0xF);
    h[25] = (uint8_t)volume;
    mb_put16(h + 26, repeat_words);
    mb_put16(h + 28, replen_words);
    m->sample_bytes += len_words * 2;
}

static inline void mb_note(modbuild_t *m, int pattern, int row, int channel, unsigned period,
                    unsigned sample, unsigned effect, unsigned param)
{
    uint8_t *p = m->buf + MB_HEADER + pattern * 1024 + row * 16 + channel * 4;
    p[0] = (uint8_t)((sample & 0xF0) | ((period >> 8) & 0x0F));
    p[1] = (uint8_t)period;
    p[2] = (uint8_t)(((sample & 0x0F) << 4) | (effect & 0x0F));
    p[3] = (uint8_t)param;
}

/* Offset of sample data in the file (and in song memory). */
static inline uint32_t mb_samples_offset(const modbuild_t *m)
{
    return MB_HEADER + (uint32_t)m->num_patterns * 1024;
}

/* Fills sample data with a recognisable non-zero pattern; returns file size. */
static inline uint32_t mb_finish(modbuild_t *m)
{
    uint32_t off = mb_samples_offset(m);
    for (uint32_t i = 0; i < m->sample_bytes; i++)
        m->buf[off + i] = (uint8_t)(i + 1);
    return off + m->sample_bytes;
}

#endif
