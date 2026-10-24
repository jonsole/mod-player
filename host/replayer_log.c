/* replayer_log: prints our Replayer's Paula writes for a MOD file, in the
 * format shared with pt2_log (see paula_log.h).
 *
 *   replayer_log song.mod ticks
 */
#include <stdio.h>
#include <stdlib.h>
#include "mod.h"
#include "paula_log.h"
#include "replayer.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: replayer_log song.mod ticks\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "replayer_log: cannot read %s\n", argv[1]);
        return 1;
    }
    static uint8_t mem[0x1000000];
    uint32_t size = (uint32_t)fread(mem, 1, sizeof mem, f);
    fclose(f);

    static mod_song_t song;
    mod_error_t err = mod_load(&song, mem, size);
    if (err != MOD_OK) {
        fprintf(stderr, "replayer_log: %s: %s\n", argv[1], mod_error_text(err));
        return 1;
    }
    static paula_log_t log;
    static replayer_t rp;
    uint32_t null_addr = 0xFFFFFFF0u; /* never a real address in this tool */
    paula_log_init(&log, stdout, song.sample[0].offset, null_addr);
    replayer_start(&rp, &song, mem, 0, null_addr, paula_log_port(&log));
    paula_log_start(&log);
    unsigned ticks = (unsigned)atoi(argv[2]);
    for (unsigned t = 1; t <= ticks && !rp.stopped; t++) {
        uint32_t cck = replayer_tick(&rp);
        paula_log_tick(&log, t, rp.play_pos, rp.play_row, rp.bpm, cck);
    }
    for (int i = 0; i < MOD_SAMPLES; i++)
        paula_log_sample(&log, i + 1, mem + song.sample[i].offset,
                         (uint32_t)song.sample[i].length * 2);
    return 0;
}
