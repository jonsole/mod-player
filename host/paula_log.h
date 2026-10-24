/* A Paula port adapter that prints every register write as text, one block
 * per tick. Used to compare our Replayer with pt2-clone's (ADR-0007): both
 * sides print through this same code, so only behaviour can differ.
 *
 *   tick 7 pos 0 row 1 bpm 125 cck 70940
 *     AUD0PER 428
 *     AUD0PTR +2108      offset into the Song's sample data, or "null"
 *     DMACON 8000
 */
#ifndef PAULA_LOG_H
#define PAULA_LOG_H

#include <stdint.h>
#include <stdio.h>
#include "paula_port.h"

typedef struct {
    FILE *out;
    uint32_t samples_start;  /* Paula address of the first Sample's data */
    uint32_t null_addr;      /* Paula address that means "no Sample" */
    uint8_t *mem;            /* memory image that PAULA_INVERT changes (Paula address 0) */
    uint32_t mem_size;
    char buf[65536];         /* this tick's writes, printed after its header */
    size_t len;
} paula_log_t;

void paula_log_init(paula_log_t *log, FILE *out, uint32_t samples_start, uint32_t null_addr);
/* Lets PAULA_INVERT writes change mem, as Paula would; they are not printed,
 * because pt2-clone changes memory directly. The Sample fingerprints at the
 * end of the log compare the result. */
void paula_log_memory(paula_log_t *log, uint8_t *mem, uint32_t mem_size);
void paula_log_write(paula_log_t *log, uint32_t reg, uint32_t value);
void paula_log_filter(paula_log_t *log, int on);

/* Prints the writes made while starting playback. */
void paula_log_start(paula_log_t *log);
/* Prints a tick's header (the Position and Row it played, its tempo and its
 * length in colour clocks) followed by the writes it made. */
void paula_log_tick(paula_log_t *log, unsigned tick, unsigned pos, unsigned row, unsigned bpm,
                    uint32_t cck);

/* Prints a fingerprint of one Sample's data (1..31), so changes the
 * Replayer makes to Sample memory (invert loop) are compared too. */
void paula_log_sample(paula_log_t *log, int number, const uint8_t *data, uint32_t len);

/* Both sides fingerprint at most this many bytes per Sample (ProTracker's
 * Sample size limit). */
#define PAULA_LOG_SAMPLE_MAX 65534u

paula_port_t paula_log_port(paula_log_t *log);

#endif
