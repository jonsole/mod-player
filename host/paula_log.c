#include <stdarg.h>
#include "paula_log.h"

void paula_log_init(paula_log_t *log, FILE *out, uint32_t samples_start, uint32_t null_addr)
{
    log->out = out;
    log->samples_start = samples_start;
    log->null_addr = null_addr;
    log->len = 0;
    log->mem = 0;
    log->mem_size = 0;
}

void paula_log_memory(paula_log_t *log, uint8_t *mem, uint32_t mem_size)
{
    log->mem = mem;
    log->mem_size = mem_size;
}

static void add(paula_log_t *log, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(log->buf + log->len, sizeof log->buf - log->len, fmt, ap);
    va_end(ap);
    if (n > 0 && log->len + (size_t)n < sizeof log->buf)
        log->len += (size_t)n;
}

void paula_log_write(paula_log_t *log, uint32_t reg, uint32_t value)
{
    static const char *names[4] = {"PTR", "LEN", "PER", "VOL"};
    if (reg == PAULA_DMACON) {
        add(log, "  DMACON %04X\n", value & 0xFFFF);
        return;
    }
    if (reg == PAULA_INVERT) {
        if (value < log->mem_size)
            log->mem[value] = (uint8_t)~log->mem[value];
        return;
    }
    if (reg == PAULA_LED_FILTER) {
        paula_log_filter(log, (int)value);
        return;
    }
    unsigned voice = reg >> 4, which = (reg & 0xF) >> 2;
    if (reg & 0xFFFFFFC3u || voice >= PAULA_VOICES) {
        add(log, "  REG%02X %u\n", reg, value);
    } else if ((reg & 0xF) != PAULA_PTR) {
        add(log, "  AUD%u%s %u\n", voice, names[which], value);
    } else if (value == log->null_addr) {
        add(log, "  AUD%uPTR null\n", voice);
    } else if (value >= log->samples_start) {
        add(log, "  AUD%uPTR +%u\n", voice, value - log->samples_start);
    } else {
        add(log, "  AUD%uPTR @%u\n", voice, value);
    }
}

void paula_log_filter(paula_log_t *log, int on)
{
    add(log, "  FILTER %d\n", on ? 1 : 0);
}

static void flush(paula_log_t *log)
{
    fwrite(log->buf, 1, log->len, log->out);
    log->len = 0;
}

void paula_log_start(paula_log_t *log)
{
    fprintf(log->out, "start\n");
    flush(log);
}

void paula_log_tick(paula_log_t *log, unsigned tick, unsigned pos, unsigned row, unsigned bpm,
                    uint32_t cck)
{
    fprintf(log->out, "tick %u pos %u row %u bpm %u cck %u\n", tick, pos, row, bpm, cck);
    flush(log);
}

void paula_log_sample(paula_log_t *log, int number, const uint8_t *data, uint32_t len)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    if (len > PAULA_LOG_SAMPLE_MAX)
        len = PAULA_LOG_SAMPLE_MAX;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ data[i]) * 16777619u;
    fprintf(log->out, "sample %d length %u data %08x\n", number, len, h);
}

static void port_write(void *ctx, uint32_t reg, uint32_t value)
{
    paula_log_write((paula_log_t *)ctx, reg, value);
}

paula_port_t paula_log_port(paula_log_t *log)
{
    paula_port_t port = {port_write, log};
    return port;
}
