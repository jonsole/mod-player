/* The Replayer: plays a Song by programming Paula through the Paula port,
 * one tick at a time, exactly as ProTracker 2.3 does (ADR-0002).
 *
 * Runs unchanged on the soft CPU (port = Paula's registers) and on the PC
 * (port = the Paula C model or a recorder).
 */
#ifndef REPLAYER_H
#define REPLAYER_H

#include <stdint.h>
#include "mod.h"
#include "paula_port.h"

typedef struct {
    uint16_t n_note;      /* raw period from the Pattern (with sample high bits masked off) */
    uint16_t n_cmd;       /* effect number << 8 | parameter */
    uint32_t n_start;     /* Paula addresses; REPLAYER_NO_SAMPLE until a Sample is set */
    uint32_t n_loopstart;
    uint32_t n_wavestart;
    uint16_t n_length;
    uint16_t n_replen;
    int16_t n_period;     /* signed, as in ProTracker: porta up can wrap it */
    int16_t n_wantedperiod;
    uint8_t n_toneportspeed;
    uint8_t n_toneportdirec;
    uint8_t n_glissfunk;  /* high nibble: invert-loop speed; low: glissando */
    uint8_t n_finetune;
    uint8_t n_volume;
    uint8_t n_dmabit;
    uint8_t n_wavecontrol;
    uint8_t n_vibratopos;
    uint8_t n_tremolopos;
    uint8_t n_vibratocmd;
    uint8_t n_tremolocmd;
    uint8_t n_sampleoffset; /* 9xx memory */
    uint8_t n_funkoffset;   /* invert-loop counter */
    uint8_t n_pattpos;    /* pattern loop start row (E60) */
    uint8_t n_loopcount;
} replayer_channel_t;

typedef struct {
    const mod_song_t *song;
    uint8_t *mem;            /* Song memory as the CPU sees it */
    uint32_t paula_base;     /* Paula's address of mem[0] */
    uint32_t null_addr;      /* Paula address of 128 KB of zeros */
    paula_port_t port;
    replayer_channel_t ch[MOD_CHANNELS];
    uint8_t speed, tick, row, pos;
    uint8_t play_pos, play_row; /* the Position and Row most recently read */
    uint8_t pbreak_pos;
    uint8_t pos_jump;        /* move to a new Position after this row (Bxx/Dxx) */
    uint8_t pbreak_flag;     /* jump to pbreak_pos in this Pattern (E6x) */
    uint8_t cia_set_bpm;     /* tempo change waiting for the next tick; 0 = none */
    uint8_t wrapped;         /* passed the last Position since the last end check */
    uint8_t visited[128 * MOD_ROWS / 8]; /* Rows played, for end-of-Song */
    uint8_t patt_del_time, patt_del_time2;
    uint8_t bpm;
    uint16_t dmacon_temp;
    uint8_t low_mask;        /* fine portamento limits the slide to 0..15 */
    uint8_t stopped;         /* F00 stopped the Song: ticks do nothing */
    uint8_t ended;           /* the Song has played through once (set, never cleared) */
} replayer_t;

#define REPLAYER_NO_SAMPLE 0xFFFFFFFFu

/* Silences Paula and gets ready to play the Song from its first Position.
 * mem is the Song memory the Song was loaded from; Paula sees mem[0] at
 * paula_base. null_addr is where Paula can read 128 KB of zeros. */
void replayer_start(replayer_t *r, const mod_song_t *song, uint8_t *mem, uint32_t paula_base,
                    uint32_t null_addr, paula_port_t port);

/* Runs one tick. Returns the number of Paula colour clocks until the next
 * tick should run.
 *
 * After a tick, `ended` is set once the next tick would replay a Row that
 * has already played (or wrap past the last Position): the Song has played
 * through. Playback carries on looping, as ProTracker does; a player in
 * "play once" mode stops here. `stopped` is set when an F00 effect stops the
 * Song; further ticks then do nothing. */
uint32_t replayer_tick(replayer_t *r);

#endif
