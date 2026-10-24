#include "wav.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void header(uint8_t h[44], uint32_t rate, uint32_t frames)
{
    static const uint8_t base[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0, /* PCM, 2 channels */
        0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0,         /* block 4, 16-bit */
        'd', 'a', 't', 'a', 0, 0, 0, 0,
    };
    for (int i = 0; i < 44; i++)
        h[i] = base[i];
    put32(h + 4, 36 + frames * 4);
    put32(h + 24, rate);
    put32(h + 28, rate * 4);
    put32(h + 40, frames * 4);
}

int wav_open(wav_writer_t *w, const char *path, uint32_t rate)
{
    uint8_t h[44];
    w->rate = rate;
    w->frames = 0;
    w->f = fopen(path, "wb");
    if (!w->f)
        return -1;
    header(h, rate, 0);
    return fwrite(h, 1, 44, w->f) == 44 ? 0 : -1;
}

void wav_frame(wav_writer_t *w, int16_t left, int16_t right)
{
    uint8_t b[4] = {(uint8_t)left, (uint8_t)((uint16_t)left >> 8), (uint8_t)right,
                    (uint8_t)((uint16_t)right >> 8)};
    fwrite(b, 1, 4, w->f);
    w->frames++;
}

int wav_close(wav_writer_t *w)
{
    uint8_t h[44];
    header(h, w->rate, w->frames);
    int bad = fseek(w->f, 0, SEEK_SET) != 0 || fwrite(h, 1, 44, w->f) != 44;
    bad |= fclose(w->f) != 0;
    return bad ? -1 : 0;
}
