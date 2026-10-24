/* Follows the structure of ProTracker 2.3's replay routine (via pt2-clone's
 * C port), so behaviour and the order of Paula writes match it exactly.
 * Function names mirror the original's to make side-by-side checks easy. */
#include "replayer.h"
#include "periods.h"

#define CIA_TIMER_BASE 1773447u /* CIA timer value at 1 BPM */
#define CCK_PER_CIA_TICK 5u

static void paula_write(replayer_t *r, uint32_t reg, uint32_t value)
{
    r->port.write(r->port.ctx, reg, value);
}

static void write_voice(replayer_t *r, int v, uint32_t reg, uint32_t value)
{
    paula_write(r, PAULA_VOICE(v) + reg, value);
}

static void write_ptr(replayer_t *r, int v, uint32_t addr)
{
    write_voice(r, v, PAULA_PTR, addr == REPLAYER_NO_SAMPLE ? r->null_addr : addr);
}

static const uint8_t *current_row(const replayer_t *r)
{
    const mod_song_t *s = r->song;
    return r->mem + s->pattern_offset + (uint32_t)s->order[r->pos] * MOD_PATTERN_BYTES +
           (uint32_t)r->row * (MOD_CHANNELS * 4);
}

static void set_speed(replayer_t *r, replayer_channel_t *c)
{
    uint8_t param = c->n_cmd & 0xFF;
    if (param > 0 && param < 32) {
        r->speed = param;
        r->tick = 0;
    }
}

/* Effects handled on the tick a row is read. */
static void check_more_effects(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    switch ((c->n_cmd >> 8) & 0xF) {
    case 0xF:
        set_speed(r, c);
        return;
    default:
        break;
    }
    write_voice(r, v, PAULA_PER, c->n_period);
}

/* Effects handled on the other ticks of a row. */
static void check_effects(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    if ((c->n_cmd & 0xFFF) != 0)
        write_voice(r, v, PAULA_PER, c->n_period);
    write_voice(r, v, PAULA_VOL, c->n_volume);
}

static void set_period(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    uint16_t note = c->n_note & 0xFFF;
    int i;
    for (i = 0; i < 37; i++)
        if (note >= replayer_periods[i]) /* entry 36 is 0, so this always stops */
            break;
    c->n_period = (uint16_t)replayer_periods[c->n_finetune * 37 + i];

    if ((c->n_cmd & 0xFF0) != 0xED0) { /* not a note delay */
        paula_write(r, PAULA_DMACON, c->n_dmabit);
        if ((c->n_wavecontrol & 0x04) == 0)
            c->n_vibratopos = 0;
        if ((c->n_wavecontrol & 0x40) == 0)
            c->n_tremolopos = 0;
        write_voice(r, v, PAULA_LEN, c->n_length);
        write_ptr(r, v, c->n_start);
        if (c->n_start == REPLAYER_NO_SAMPLE) {
            c->n_loopstart = REPLAYER_NO_SAMPLE;
            write_voice(r, v, PAULA_LEN, 1);
            c->n_replen = 1;
        }
        write_voice(r, v, PAULA_PER, c->n_period);
        r->dmacon_temp |= c->n_dmabit;
    }
    check_more_effects(r, v);
}

static void play_voice(replayer_t *r, int v, const uint8_t *cell)
{
    replayer_channel_t *c = &r->ch[v];
    if (c->n_note == 0 && c->n_cmd == 0)
        write_voice(r, v, PAULA_PER, c->n_period);

    c->n_note = (uint16_t)(((cell[0] & 0x0F) << 8) | cell[1]);
    c->n_cmd = (uint16_t)(((cell[2] & 0x0F) << 8) | cell[3]);
    unsigned sample = ((cell[0] & 0xF0) | (cell[2] >> 4)) & 31;

    if (sample >= 1) {
        const mod_sample_t *s = &r->song->sample[sample - 1];
        c->n_start = r->paula_base + s->offset;
        c->n_finetune = s->finetune;
        c->n_volume = s->volume;
        c->n_length = s->length;
        c->n_replen = s->replen;
        if (s->repeat > 0) {
            c->n_loopstart = c->n_start + (uint32_t)s->repeat * 2;
            c->n_wavestart = c->n_loopstart;
            c->n_length = (uint16_t)(s->repeat + c->n_replen);
        } else {
            c->n_loopstart = c->n_start;
            c->n_wavestart = c->n_start;
        }
        if (c->n_length == 0) /* empty Sample: point at silence */
            c->n_loopstart = c->n_wavestart = r->null_addr;
    }

    if ((c->n_note & 0xFFF) > 0)
        set_period(r, v);
    else
        check_more_effects(r, v);
}

static void set_dma(replayer_t *r)
{
    paula_write(r, PAULA_DMACON, PAULA_DMACON_SET | r->dmacon_temp);
    for (int v = 0; v < MOD_CHANNELS; v++) {
        write_ptr(r, v, r->ch[v].n_loopstart);
        write_voice(r, v, PAULA_LEN, r->ch[v].n_replen);
    }
}

static void next_position(replayer_t *r)
{
    r->row = r->pbreak_pos;
    r->pbreak_pos = 0;
    r->pos_jump = 0;
    r->pos = (uint8_t)((r->pos + 1) & 127);
    if (r->pos >= r->song->length)
        r->pos = 0;
}

void replayer_start(replayer_t *r, const mod_song_t *song, uint8_t *mem, uint32_t paula_base,
                    uint32_t null_addr, paula_port_t port)
{
    *r = (replayer_t){0};
    r->song = song;
    r->mem = mem;
    r->paula_base = paula_base;
    r->null_addr = null_addr;
    r->port = port;
    r->speed = 6;
    r->tick = r->speed - 1; /* so the first tick reads row 0 */
    r->bpm = 125;
    for (int v = 0; v < MOD_CHANNELS; v++) {
        r->ch[v].n_dmabit = (uint8_t)(1u << v);
        r->ch[v].n_start = r->ch[v].n_loopstart = r->ch[v].n_wavestart = REPLAYER_NO_SAMPLE;
    }
    paula_write(r, PAULA_DMACON, 0xF);
    for (int v = 0; v < MOD_CHANNELS; v++)
        write_voice(r, v, PAULA_VOL, 0);
}

uint32_t replayer_tick(replayer_t *r)
{
    if (++r->tick >= r->speed) {
        r->tick = 0;
        if (r->patt_del_time2 == 0) {
            r->dmacon_temp = 0;
            const uint8_t *row = current_row(r);
            for (int v = 0; v < MOD_CHANNELS; v++) {
                play_voice(r, v, row + v * 4);
                write_voice(r, v, PAULA_VOL, r->ch[v].n_volume);
            }
            set_dma(r);
        } else {
            for (int v = 0; v < MOD_CHANNELS; v++)
                check_effects(r, v);
        }
        r->row++;
        if (r->row >= MOD_ROWS || r->pos_jump)
            next_position(r);
    } else {
        for (int v = 0; v < MOD_CHANNELS; v++)
            check_effects(r, v);
        if (r->pos_jump)
            next_position(r);
    }
    return (CIA_TIMER_BASE / r->bpm) * CCK_PER_CIA_TICK;
}
