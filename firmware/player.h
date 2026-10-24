/* Plays the Song in SDRAM through Paula (ADR-0009): wait for Paula's tick
 * flag, run one Replayer tick (its writes queue up in Paula), give Paula
 * the next tick's length. */
#ifndef PLAYER_H
#define PLAYER_H
#include <stdint.h>
#include "mod.h"

/* Bytes of zeros kept after the Song for Paula to play as silence. */
#define PLAYER_SILENCE_BYTES (128u * 1024u)

void player_init(void);
/* Checks the MOD file at mem (CPU address); NULL if good, else why not. */
const char *player_load(uint8_t *mem, uint32_t size);
const mod_song_t *player_song(void);   /* NULL if none loaded */
void player_play(void);                /* from the start */
void player_stop(void);                /* and silence */
int player_pause_toggle(void);         /* returns 1 if now paused */
void player_poll(void);                /* call often */

#endif
