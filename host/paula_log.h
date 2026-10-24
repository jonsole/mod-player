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
    char buf[65536];         /* this tick's writes, printed after its header */
    size_t len;
} paula_log_t;

void paula_log_init(paula_log_t *log, FILE *out, uint32_t samples_start, uint32_t null_addr);
void paula_log_write(paula_log_t *log, uint32_t reg, uint32_t value);
void paula_log_filter(paula_log_t *log, int on);

/* Prints the writes made while starting playback. */
void paula_log_start(paula_log_t *log);
/* Prints a tick's header (the Position and Row it played, its tempo and its
 * length in colour clocks) followed by the writes it made. */
void paula_log_tick(paula_log_t *log, unsigned tick, unsigned pos, unsigned row, unsigned bpm,
                    uint32_t cck);

paula_port_t paula_log_port(paula_log_t *log);

#endif
