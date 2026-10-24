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
    uint16_t n_period;
    uint8_t n_finetune;
    uint8_t n_volume;
    uint8_t n_dmabit;
    uint8_t n_wavecontrol;
    uint8_t n_vibratopos;
    uint8_t n_tremolopos;
    uint8_t n_vibratocmd;
    uint8_t n_tremolocmd;
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
    uint8_t pos_jump;
    uint8_t patt_del_time, patt_del_time2;
    uint8_t bpm;
    uint16_t dmacon_temp;
} replayer_t;

#define REPLAYER_NO_SAMPLE 0xFFFFFFFFu

/* Silences Paula and gets ready to play the Song from its first Position.
 * mem is the Song memory the Song was loaded from; Paula sees mem[0] at
 * paula_base. null_addr is where Paula can read 128 KB of zeros. */
void replayer_start(replayer_t *r, const mod_song_t *song, uint8_t *mem, uint32_t paula_base,
                    uint32_t null_addr, paula_port_t port);

/* Runs one tick. Returns the number of Paula colour clocks until the next
 * tick should run. */
uint32_t replayer_tick(replayer_t *r);

#endif
