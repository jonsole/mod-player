/* Dumps each Voice's output (sample x volume, 0 when off) per colour clock
 * from the Paula C model, for tools/scope_preview.py.
 *
 *   voice_dump song.mod start_seconds length_seconds out.bin
 *
 * out.bin: four little-endian int16 per colour clock (Voice 1..4). Build:
 *   cc -O2 -Ihost -Ifirmware/replayer -o voice_dump tools/voice_dump.c \
 *      host/paula_model.c firmware/replayer/mod.c firmware/replayer/periods.c firmware/replayer/replayer.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mod.h"
#include "paula_model.h"
#include "replayer.h"

#define CCK_HZ 3546895.0

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: voice_dump song.mod start_seconds length_seconds out.bin\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t null_addr = (size + 1) & ~1u;
    uint32_t mem_size = null_addr + 131072;
    uint8_t *mem = calloc(mem_size, 1);
    if (fread(mem, 1, size, f) != size) return 2;
    fclose(f);

    static mod_song_t song;
    if (mod_load(&song, mem, size) != MOD_OK) { fprintf(stderr, "not a MOD file\n"); return 2; }
    static paula_model_t paula;
    static replayer_t rp;
    paula_model_reset(&paula, mem, mem_size);
    replayer_start(&rp, &song, mem, 0, null_addr, paula_model_port(&paula));

    long skip = (long)(atof(argv[2]) * CCK_HZ), keep = (long)(atof(argv[3]) * CCK_HZ);
    FILE *out = fopen(argv[4], "wb");
    if (!out) return 2;
    uint32_t tick_left = 0;
    for (long n = 0; n < skip + keep; n++) {
        if (tick_left == 0)
            tick_left = rp.stopped ? 70940 : replayer_tick(&rp);
        tick_left--;
        int16_t v[4];
        for (int k = 0; k < 4; k++)
            v[k] = (int16_t)(paula.voice[k].on ? paula.voice[k].out : 0);
        if (n >= skip)
            fwrite(v, sizeof v, 1, out);
        paula_model_step(&paula);
    }
    fclose(out);
    return 0;
}
