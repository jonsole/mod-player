#include <string.h>
#include "paula_model.h"

void paula_model_reset(paula_model_t *p, const uint8_t *mem, uint32_t mem_size)
{
    memset(p, 0, sizeof *p);
    p->mem = mem;
    p->mem_size = mem_size;
    for (int v = 0; v < PAULA_VOICES; v++)
        p->voice[v].per = 65536;
}

static int8_t read_byte(const paula_model_t *p, uint32_t addr)
{
    return addr < p->mem_size ? (int8_t)p->mem[addr] : 0;
}

/* One sample step: fetch a new word when the last one is used up, then
 * play its next byte at the current volume. */
static void next_sample(paula_model_t *p, paula_voice_t *v)
{
    if (v->bytes_left == 0) {
        if (!v->just_started && --v->len_left == 0) {
            v->len_left = v->len;
            v->location = v->ptr;
        }
        v->just_started = 0;
        v->dat[0] = read_byte(p, v->location);
        v->dat[1] = read_byte(p, v->location + 1);
        v->location += 2;
        v->bytes_left = 2;
    }
    v->out = (int32_t)v->dat[0] * v->vol;
    v->dat[0] = v->dat[1];
    v->bytes_left--;
}

static void start_voice(paula_model_t *p, paula_voice_t *v)
{
    v->location = v->ptr;
    v->len_left = v->len;
    v->bytes_left = 0;
    v->just_started = 1;
    v->per_left = v->per;
    v->on = 1;
    next_sample(p, v);
}

void paula_model_write(paula_model_t *p, uint32_t reg, uint32_t value)
{
    if (reg == PAULA_DMACON) {
        for (int i = 0; i < PAULA_VOICES; i++) {
            if (!(value & (1u << i)))
                continue;
            if (value & PAULA_DMACON_SET)
                start_voice(p, &p->voice[i]);
            else
                p->voice[i].on = 0;
        }
        return;
    }
    if (reg >= PAULA_VOICE(PAULA_VOICES))
        return;
    paula_voice_t *v = &p->voice[reg >> 4];
    switch (reg & 0xF) {
    case PAULA_PTR:
        v->ptr = value & ~1u;
        break;
    case PAULA_LEN:
        v->len = (uint16_t)value;
        break;
    case PAULA_PER:
        value &= 0xFFFF;
        v->per = value == 0 ? 65536 : value < 113 ? 113 : value;
        break;
    case PAULA_VOL:
        value &= 0x7F;
        v->vol = (uint8_t)(value > 64 ? 64 : value);
        break;
    }
}

paula_frame_t paula_model_step(paula_model_t *p)
{
    int32_t out[PAULA_VOICES];
    for (int i = 0; i < PAULA_VOICES; i++) {
        paula_voice_t *v = &p->voice[i];
        out[i] = v->on ? v->out : 0;
        if (v->on && --v->per_left == 0) {
            v->per_left = v->per;
            next_sample(p, v);
        }
    }
    paula_frame_t f = {out[0] + out[3], out[1] + out[2]};
    return f;
}

static void port_write(void *ctx, uint32_t reg, uint32_t value)
{
    paula_model_write((paula_model_t *)ctx, reg, value);
}

paula_port_t paula_model_port(paula_model_t *p)
{
    paula_port_t port = {port_write, p};
    return port;
}
