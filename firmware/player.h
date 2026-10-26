/* Plays the Song in SDRAM through Paula (ADR-0009): wait for Paula's tick
 * flag, run one Replayer tick (its writes queue up in Paula), give Paula
 * the next tick's length. */
#ifndef PLAYER_H
#define PLAYER_H
#include <stdint.h>
#include "mod.h"

/* Bytes of zeros kept after the Song for Paula to play as silence. */
#define PLAYER_SILENCE_BYTES (128u * 1024u)

/* After a Song stops (F00, or its end in play-once mode) the voices are
 * switched off at the next tick boundary and Paula keeps ticking at this
 * length (125 BPM). render and the simulation reference do the same. */
#define PLAYER_SILENT_TICK 70940u

void player_init(void);
/* Checks the MOD file at mem (CPU address); NULL if good, else why not. */
const char *player_load(uint8_t *mem, uint32_t size);
const mod_song_t *player_song(void);   /* NULL if none loaded */
void player_play(void);                /* from the start */
void player_stop(void);                /* and silence */
int player_pause_toggle(void);         /* returns 1 if now paused */
void player_poll(void);                /* call often */
int player_playing(void);
int player_paused(void);

/* Where the Song is, as heard: the Replayer runs a tick ahead of Paula
 * (ADR-0009), so this is its state from before it ran the next tick. */
typedef struct {
    uint8_t pos, pattern, row, speed, bpm;
} player_position_t;
/* 0 if no Song has played since it was loaded. */
int player_position(player_position_t *p);
/* Changes each time a Song is loaded, so a screen can tell. */
uint32_t player_load_count(void);
/* Bring-up check: does Paula's tick timer fire? Returns 1 if so. */
int player_self_test(void);

#endif
