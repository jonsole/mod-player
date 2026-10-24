/* render: plays a MOD file through the Replayer and the Paula C model and
 * writes the result as a 48 kHz stereo WAV file.
 *
 *   render in.mod out.wav [--seconds N] [--play-once] [--blend] [--a500] [--no-led]
 *
 * Renders 30 seconds by default. --play-once stops at the end of the Song
 * (or at F00), whichever comes first with --seconds. --blend, --a500 and
 * --no-led set Paula's output options (see paula_port.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mod.h"
#include "paula_model.h"
#include "replayer.h"
#include "wav.h"

#define OUT_RATE 48000u
#define NULL_SAMPLE_BYTES (65536u * 2)

static int usage(void)
{
    fprintf(stderr, "usage: render in.mod out.wav [--seconds N] [--play-once] [--blend] "
                    "[--a500] [--no-led]\n");
    return 2;
}

/* Reads the whole file, leaving NULL_SAMPLE_BYTES of zeros after it (rounded
 * to an even address) for Paula to play as silence. */
static uint8_t *read_song_memory(const char *path, uint32_t *size, uint32_t *null_addr)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > 0x1000000) {
        fclose(f);
        return NULL;
    }
    *size = (uint32_t)len;
    *null_addr = (*size + 1) & ~1u;
    uint8_t *mem = calloc(1, *null_addr + NULL_SAMPLE_BYTES);
    if (mem && fread(mem, 1, *size, f) != *size) {
        free(mem);
        mem = NULL;
    }
    fclose(f);
    return mem;
}

/* Output is the average of the colour-clock frames in each output sample,
 * scaled from Paula's range (+-16384 per side) to 16 bits. */
static int16_t to_pcm(int64_t sum, uint32_t count)
{
    int64_t v = sum * 2 / (int64_t)count;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    double seconds = -1;
    int play_once = 0;
    uint32_t options = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--play-once") == 0)
            play_once = 1;
        else if (strcmp(argv[i], "--blend") == 0)
            options |= PAULA_OPT_BLEND;
        else if (strcmp(argv[i], "--a500") == 0)
            options |= PAULA_OPT_A500;
        else if (strcmp(argv[i], "--no-led") == 0)
            options |= PAULA_OPT_NO_LED;
        else
            return usage();
    }
    if (seconds < 0)
        seconds = play_once ? 3600 : 30;

    uint32_t size, null_addr;
    uint8_t *mem = read_song_memory(argv[1], &size, &null_addr);
    if (!mem) {
        fprintf(stderr, "render: cannot read %s\n", argv[1]);
        return 1;
    }
    static mod_song_t song;
    mod_error_t err = mod_load(&song, mem, size);
    if (err != MOD_OK) {
        fprintf(stderr, "render: %s: %s\n", argv[1], mod_error_text(err));
        return 1;
    }

    static paula_model_t paula;
    static replayer_t rp;
    paula_model_reset(&paula, mem, null_addr + NULL_SAMPLE_BYTES);
    paula_model_write(&paula, PAULA_OPTIONS, options);
    replayer_start(&rp, &song, mem, 0, null_addr, paula_model_port(&paula));

    wav_writer_t wav;
    if (wav_open(&wav, argv[2], OUT_RATE) != 0) {
        fprintf(stderr, "render: cannot write %s\n", argv[2]);
        return 1;
    }
    uint32_t frames = (uint32_t)(seconds * OUT_RATE);
    uint32_t tick_left = 0, phase = 0, count = 0;
    int64_t sum_l = 0, sum_r = 0;
    int last_tick = 0, silent = 0;
    while (wav.frames < frames) {
        if (tick_left == 0) {
            if (last_tick)
                break;
            if (silent) {
                tick_left = 70940;
            } else if (rp.stopped) {
                /* F00: as on the board, the voices go off at the next tick */
                paula_model_write(&paula, PAULA_DMACON, 0xF);
                for (int v = 0; v < PAULA_VOICES; v++)
                    paula_model_write(&paula, PAULA_VOICE(v) + PAULA_VOL, 0);
                silent = 1;
                tick_left = 70940;
            } else {
                tick_left = replayer_tick(&rp);
                last_tick = play_once && (rp.ended || rp.stopped);
            }
        }
        tick_left--;
        paula_frame_t f = paula_model_step(&paula);
        sum_l += f.left;
        sum_r += f.right;
        count++;
        phase += OUT_RATE;
        if (phase >= PAULA_CCK_HZ) {
            phase -= PAULA_CCK_HZ;
            wav_frame(&wav, to_pcm(sum_l, count), to_pcm(sum_r, count));
            sum_l = sum_r = 0;
            count = 0;
        }
    }
    if (wav_close(&wav) != 0) {
        fprintf(stderr, "render: cannot write %s\n", argv[2]);
        return 1;
    }
    free(mem);
    return 0;
}
