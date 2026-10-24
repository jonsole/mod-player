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

static uint8_t clamp_volume(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 64 ? 64 : v);
}

/* ---- volume effects ------------------------------------------------------ */

static void volume_change(replayer_t *r, replayer_channel_t *c) /* Cxx */
{
    (void)r;
    uint8_t v = c->n_cmd & 0xFF;
    c->n_volume = v > 64 ? 64 : v;
}

static void volume_slide(replayer_channel_t *c) /* Axy */
{
    uint8_t param = c->n_cmd & 0xFF;
    if ((param & 0xF0) == 0)
        c->n_volume = clamp_volume(c->n_volume - (param & 0x0F));
    else
        c->n_volume = clamp_volume(c->n_volume + (param >> 4));
}

static void volume_fine_up(replayer_t *r, replayer_channel_t *c) /* EAx */
{
    if (r->tick == 0)
        c->n_volume = clamp_volume(c->n_volume + (c->n_cmd & 0xF));
}

static void volume_fine_down(replayer_t *r, replayer_channel_t *c) /* EBx */
{
    if (r->tick == 0)
        c->n_volume = clamp_volume(c->n_volume - (c->n_cmd & 0xF));
}

static void set_tremolo_control(replayer_channel_t *c) /* E7x */
{
    c->n_wavecontrol = (uint8_t)(((c->n_cmd & 0xF) << 4) | (c->n_wavecontrol & 0xF));
}

static void tremolo(replayer_t *r, int v) /* 7xy */
{
    replayer_channel_t *c = &r->ch[v];
    if (c->n_cmd & 0x0F)
        c->n_tremolocmd = (uint8_t)((c->n_tremolocmd & 0xF0) | (c->n_cmd & 0x0F));
    if (c->n_cmd & 0xF0)
        c->n_tremolocmd = (uint8_t)((c->n_cmd & 0xF0) | (c->n_tremolocmd & 0x0F));

    unsigned pos = (c->n_tremolopos >> 2) & 0x1F;
    unsigned depth;
    switch ((c->n_wavecontrol >> 4) & 3) {
    case 0: /* sine */
        depth = replayer_vibrato_table[pos];
        break;
    case 1: /* ramp: ProTracker tests the vibrato position here (a bug we keep) */
        depth = c->n_vibratopos < 128 ? pos << 3 : 255 - (pos << 3);
        break;
    default: /* square */
        depth = 255;
        break;
    }
    depth = (depth * (c->n_tremolocmd & 0xF)) >> 6;
    int vol = c->n_tremolopos < 128 ? c->n_volume + (int)depth : c->n_volume - (int)depth;
    write_voice(r, v, PAULA_VOL, clamp_volume(vol));
    c->n_tremolopos = (uint8_t)(c->n_tremolopos + ((c->n_tremolocmd >> 2) & 0x3C));
}

/* ---- pitch effects ------------------------------------------------------- */

static const int16_t *finetune_periods(const replayer_channel_t *c)
{
    return &replayer_periods[c->n_finetune * 37];
}

static void arpeggio(replayer_t *r, int v) /* 0xy */
{
    replayer_channel_t *c = &r->ch[v];
    unsigned arp;
    switch (r->tick % 3) {
    case 1:
        arp = (c->n_cmd >> 4) & 0xF;
        break;
    case 2:
        arp = c->n_cmd & 0xF;
        break;
    default:
        write_voice(r, v, PAULA_PER, (uint16_t)c->n_period);
        return;
    }
    /* At finetune -1 this can read up to 15 entries past the table, which
     * holds ProTracker's own overflow values for exactly this case. */
    const int16_t *periods = finetune_periods(c);
    for (int i = 0; i < 37; i++) {
        if (c->n_period >= periods[i]) {
            write_voice(r, v, PAULA_PER, (uint16_t)periods[i + arp]);
            return;
        }
    }
}

static void porta_up(replayer_t *r, int v) /* 1xx */
{
    replayer_channel_t *c = &r->ch[v];
    c->n_period = (int16_t)(c->n_period - ((c->n_cmd & 0xFF) & r->low_mask));
    r->low_mask = 0xFF;
    /* ProTracker bug: the sign is dropped before the check, so an underflow
     * is not clamped. */
    if ((c->n_period & 0xFFF) < 113)
        c->n_period = (int16_t)((c->n_period & 0xF000) | 113);
    write_voice(r, v, PAULA_PER, (uint16_t)c->n_period & 0xFFF);
}

static void porta_down(replayer_t *r, int v) /* 2xx */
{
    replayer_channel_t *c = &r->ch[v];
    c->n_period = (int16_t)(c->n_period + ((c->n_cmd & 0xFF) & r->low_mask));
    r->low_mask = 0xFF;
    if ((c->n_period & 0xFFF) > 856)
        c->n_period = (int16_t)((c->n_period & 0xF000) | 856);
    write_voice(r, v, PAULA_PER, (uint16_t)c->n_period & 0xFFF);
}

static void fine_porta_up(replayer_t *r, int v) /* E1x */
{
    if (r->tick == 0) {
        r->low_mask = 0xF;
        porta_up(r, v);
    }
}

static void fine_porta_down(replayer_t *r, int v) /* E2x */
{
    if (r->tick == 0) {
        r->low_mask = 0xF;
        porta_down(r, v);
    }
}

/* Index of the first table entry not above `period`, as ProTracker finds
 * it for tone portamento (35 if none). */
static int porta_index(const int16_t *periods, int period)
{
    for (int i = 0; i < 37; i++)
        if (period >= periods[i])
            return i;
    return 35;
}

static void set_tone_porta(replayer_channel_t *c) /* the note of a 3xx/5xy row */
{
    const int16_t *periods = finetune_periods(c);
    int i = porta_index(periods, c->n_note & 0xFFF);
    if ((c->n_finetune & 8) && i > 0)
        i--;
    c->n_wantedperiod = periods[i];
    c->n_toneportdirec = 0;
    if (c->n_period == c->n_wantedperiod)
        c->n_wantedperiod = 0;
    else if (c->n_period > c->n_wantedperiod)
        c->n_toneportdirec = 1;
}

static void tone_port_no_change(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    if (c->n_wantedperiod <= 0)
        return;
    if (c->n_toneportdirec) {
        c->n_period = (int16_t)(c->n_period - c->n_toneportspeed);
        if (c->n_period <= c->n_wantedperiod) {
            c->n_period = c->n_wantedperiod;
            c->n_wantedperiod = 0;
        }
    } else {
        c->n_period = (int16_t)(c->n_period + c->n_toneportspeed);
        if (c->n_period >= c->n_wantedperiod) {
            c->n_period = c->n_wantedperiod;
            c->n_wantedperiod = 0;
        }
    }
    if ((c->n_glissfunk & 0xF) == 0) {
        write_voice(r, v, PAULA_PER, (uint16_t)c->n_period);
    } else { /* glissando: round to a note */
        const int16_t *periods = finetune_periods(c);
        write_voice(r, v, PAULA_PER, (uint16_t)periods[porta_index(periods, c->n_period)]);
    }
}

static void tone_portamento(replayer_t *r, int v) /* 3xx */
{
    replayer_channel_t *c = &r->ch[v];
    if (c->n_cmd & 0xFF) {
        c->n_toneportspeed = c->n_cmd & 0xFF;
        c->n_cmd &= 0xFF00;
    }
    tone_port_no_change(r, v);
}

static void vibrato2(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    unsigned pos = (c->n_vibratopos >> 2) & 0x1F;
    unsigned depth;
    switch (c->n_wavecontrol & 3) {
    case 0: /* sine */
        depth = replayer_vibrato_table[pos];
        break;
    case 1: /* ramp */
        depth = c->n_vibratopos < 128 ? pos << 3 : 255 - (pos << 3);
        break;
    default: /* square */
        depth = 255;
        break;
    }
    depth = (depth * (c->n_vibratocmd & 0xF)) >> 7;
    uint16_t period = c->n_vibratopos < 128 ? (uint16_t)(c->n_period + depth)
                                            : (uint16_t)(c->n_period - depth);
    write_voice(r, v, PAULA_PER, period);
    c->n_vibratopos = (uint8_t)(c->n_vibratopos + ((c->n_vibratocmd >> 2) & 0x3C));
}

static void vibrato(replayer_t *r, int v) /* 4xy */
{
    replayer_channel_t *c = &r->ch[v];
    if (c->n_cmd & 0x0F)
        c->n_vibratocmd = (uint8_t)((c->n_vibratocmd & 0xF0) | (c->n_cmd & 0x0F));
    if (c->n_cmd & 0xF0)
        c->n_vibratocmd = (uint8_t)((c->n_cmd & 0xF0) | (c->n_vibratocmd & 0x0F));
    vibrato2(r, v);
}

static void set_gliss_control(replayer_channel_t *c) /* E3x */
{
    c->n_glissfunk = (uint8_t)((c->n_glissfunk & 0xF0) | (c->n_cmd & 0x0F));
}

static void set_vibrato_control(replayer_channel_t *c) /* E4x */
{
    c->n_wavecontrol = (uint8_t)((c->n_wavecontrol & 0xF0) | (c->n_cmd & 0x0F));
}

/* ---- Sample effects ------------------------------------------------------ */

static void sample_offset(replayer_channel_t *c) /* 9xx */
{
    if (c->n_cmd & 0xFF)
        c->n_sampleoffset = c->n_cmd & 0xFF;
    uint16_t offset = (uint16_t)(c->n_sampleoffset << 7); /* in words */
    if (offset < c->n_length) {
        c->n_length = (uint16_t)(c->n_length - offset);
        c->n_start += (uint32_t)offset << 1;
    } else {
        c->n_length = 1;
    }
}

/* Restart the voice's Sample now (retrigger and note delay). */
static void do_retrig(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    paula_write(r, PAULA_DMACON, c->n_dmabit);
    write_ptr(r, v, c->n_start);
    write_voice(r, v, PAULA_LEN, c->n_length);
    write_voice(r, v, PAULA_PER, (uint16_t)c->n_period);
    paula_write(r, PAULA_DMACON, PAULA_DMACON_SET | c->n_dmabit);
    write_ptr(r, v, c->n_loopstart);
    write_voice(r, v, PAULA_LEN, c->n_replen);
}

static void retrig_note(replayer_t *r, int v) /* E9x */
{
    replayer_channel_t *c = &r->ch[v];
    unsigned x = c->n_cmd & 0xF;
    if (x == 0 || (r->tick == 0 && (c->n_note & 0xFFF)))
        return;
    if (r->tick % x == 0)
        do_retrig(r, v);
}

static void note_cut(replayer_t *r, replayer_channel_t *c) /* ECx */
{
    if (r->tick == (c->n_cmd & 0xF))
        c->n_volume = 0;
}

static void note_delay(replayer_t *r, int v) /* EDx */
{
    replayer_channel_t *c = &r->ch[v];
    if (r->tick == (c->n_cmd & 0xF) && (c->n_note & 0xFFF))
        do_retrig(r, v);
}

static void set_fine_tune(replayer_channel_t *c) /* E5x */
{
    c->n_finetune = c->n_cmd & 0xF;
}

static void filter_on_off(replayer_t *r, replayer_channel_t *c) /* E0x: 0 = on */
{
    if (r->tick == 0)
        paula_write(r, PAULA_LED_FILTER, (c->n_cmd & 1) ^ 1);
}

static const uint8_t funk_table[16] = {0x00, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0D,
                                       0x10, 0x13, 0x16, 0x1A, 0x20, 0x2B, 0x40, 0x80};

/* Invert loop: every so often, flip one byte of the loop, walking along it. */
static void update_funk(replayer_t *r, replayer_channel_t *c)
{
    unsigned speed = c->n_glissfunk >> 4;
    if (speed == 0)
        return;
    unsigned off = c->n_funkoffset + funk_table[speed];
    c->n_funkoffset = (uint8_t)off;
    if (off < 128)
        return;
    c->n_funkoffset = 0;
    /* ProTracker would also flip bytes of the shared silent Sample; skip it */
    if (c->n_loopstart == REPLAYER_NO_SAMPLE || c->n_wavestart == REPLAYER_NO_SAMPLE ||
        c->n_loopstart == r->null_addr)
        return;
    if (++c->n_wavestart >= c->n_loopstart + ((uint32_t)c->n_replen << 1))
        c->n_wavestart = c->n_loopstart;
    /* ProTracker sets the byte to -1 - byte, which flips every bit. Paula
     * does it, in step with the tick's other writes (ADR-0009). */
    paula_write(r, PAULA_INVERT, c->n_wavestart);
}

static void funk_it(replayer_t *r, replayer_channel_t *c) /* EFx */
{
    if (r->tick != 0)
        return;
    c->n_glissfunk = (uint8_t)(((c->n_cmd & 0xF) << 4) | (c->n_glissfunk & 0xF));
    if (c->n_glissfunk & 0xF0)
        update_funk(r, c);
}

/* ---- song flow ----------------------------------------------------------- */

static void set_speed(replayer_t *r, replayer_channel_t *c) /* Fxx */
{
    uint8_t param = c->n_cmd & 0xFF;
    if (param == 0) {
        r->stopped = 1;
    } else if (param < 32) {
        r->speed = param;
        r->tick = 0;
    } else {
        r->cia_set_bpm = param; /* the CIA takes its new value at the next tick */
    }
}

static void position_jump(replayer_t *r, replayer_channel_t *c) /* Bxx */
{
    r->pos = (uint8_t)((c->n_cmd & 0xFF) - 1); /* B00 gives 255: the next Position is 0 */
    r->pbreak_pos = 0;
    r->pos_jump = 1;
}

static void pattern_break(replayer_t *r, replayer_channel_t *c) /* Dxx, decimal */
{
    unsigned row = ((c->n_cmd >> 4) & 0xF) * 10 + (c->n_cmd & 0xF);
    r->pbreak_pos = (uint8_t)(row > 63 ? 0 : row);
    r->pos_jump = 1;
}

static int visited(const replayer_t *r, unsigned pos, unsigned row)
{
    unsigned i = pos * MOD_ROWS + row;
    return (r->visited[i >> 3] >> (i & 7)) & 1;
}

static void set_visited(replayer_t *r, unsigned pos, unsigned row, int on)
{
    unsigned i = pos * MOD_ROWS + row;
    if (on)
        r->visited[i >> 3] |= (uint8_t)(1u << (i & 7));
    else
        r->visited[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static void jump_loop(replayer_t *r, replayer_channel_t *c) /* E6x */
{
    if (r->tick != 0)
        return;
    if ((c->n_cmd & 0xF) == 0) {
        c->n_pattpos = r->row;
        return;
    }
    if (c->n_loopcount == 0)
        c->n_loopcount = c->n_cmd & 0xF;
    else if (--c->n_loopcount == 0)
        return;
    r->pbreak_pos = c->n_pattpos;
    r->pbreak_flag = 1;
    /* the looped Rows will play again: not the end of the Song */
    for (unsigned row = r->pbreak_pos; row <= r->row; row++)
        set_visited(r, r->pos, row, 0);
}

static void pattern_delay(replayer_t *r, replayer_channel_t *c) /* EEx */
{
    if (r->tick == 0 && r->patt_del_time2 == 0)
        r->patt_del_time = (uint8_t)((c->n_cmd & 0xF) + 1);
}

/* ---- dispatch, in ProTracker's order ------------------------------------- */

static void e_commands(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    switch ((c->n_cmd >> 4) & 0xF) {
    case 0x0:
        filter_on_off(r, c);
        return;
    case 0x1:
        fine_porta_up(r, v);
        return;
    case 0x2:
        fine_porta_down(r, v);
        return;
    case 0x3:
        set_gliss_control(c);
        return;
    case 0x4:
        set_vibrato_control(c);
        return;
    case 0x5:
        set_fine_tune(c);
        return;
    case 0x6:
        jump_loop(r, c);
        return;
    case 0x7:
        set_tremolo_control(c);
        return;
    case 0xE:
        pattern_delay(r, c);
        return;
    case 0x8: /* Karplus-Strong: off in ProTracker's default setup */
        return;
    case 0x9:
        retrig_note(r, v);
        return;
    case 0xA:
        volume_fine_up(r, c);
        return;
    case 0xB:
        volume_fine_down(r, c);
        return;
    case 0xC:
        note_cut(r, c);
        return;
    case 0xD:
        note_delay(r, v);
        return;
    case 0xF:
        funk_it(r, c);
        return;
    default:
        return;
    }
}

/* Effects handled on the tick a row is read. */
static void check_more_effects(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    switch ((c->n_cmd >> 8) & 0xF) {
    case 0x9:
        sample_offset(c);
        return;
    case 0xB:
        position_jump(r, c);
        return;
    case 0xD:
        pattern_break(r, c);
        return;
    case 0xE:
        e_commands(r, v);
        return;
    case 0xF:
        set_speed(r, c);
        return;
    case 0xC:
        volume_change(r, c);
        return;
    default:
        write_voice(r, v, PAULA_PER, (uint16_t)c->n_period);
        return;
    }
}

/* Effects handled on the other ticks of a row. */
static void chkefx2(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    update_funk(r, c);
    if ((c->n_cmd & 0xFFF) == 0)
        return;
    unsigned cmd = (c->n_cmd >> 8) & 0xF;
    switch (cmd) {
    case 0x0:
        arpeggio(r, v);
        return;
    case 0x1:
        porta_up(r, v);
        return;
    case 0x2:
        porta_down(r, v);
        return;
    case 0x3:
        tone_portamento(r, v);
        return;
    case 0x4:
        vibrato(r, v);
        return;
    case 0x5:
        tone_port_no_change(r, v);
        volume_slide(c);
        return;
    case 0x6:
        vibrato2(r, v);
        volume_slide(c);
        return;
    case 0xE:
        e_commands(r, v);
        return;
    default:
        break;
    }
    write_voice(r, v, PAULA_PER, (uint16_t)c->n_period);
    if (cmd == 0x7)
        tremolo(r, v);
    else if (cmd == 0xA)
        volume_slide(c);
}

static void check_effects(replayer_t *r, int v)
{
    replayer_channel_t *c = &r->ch[v];
    chkefx2(r, v);
    /* tremolo writes its own volume */
    if (((c->n_cmd >> 8) & 0xF) != 0x7)
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
        write_voice(r, v, PAULA_PER, (uint16_t)c->n_period);
        r->dmacon_temp |= c->n_dmabit;
    }
    check_more_effects(r, v);
}

static void play_voice(replayer_t *r, int v, const uint8_t *cell)
{
    replayer_channel_t *c = &r->ch[v];
    if (c->n_note == 0 && c->n_cmd == 0)
        write_voice(r, v, PAULA_PER, (uint16_t)c->n_period);

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

    if ((c->n_note & 0xFFF) == 0) {
        check_more_effects(r, v);
        return;
    }
    unsigned cmd = (c->n_cmd >> 8) & 0xF;
    if ((c->n_cmd & 0xFF0) == 0xE50) {
        set_fine_tune(c);
        set_period(r, v);
    } else if (cmd == 3 || cmd == 5) { /* tone portamento: slide to the note, don't trigger it */
        set_tone_porta(c);
        check_more_effects(r, v);
    } else if (cmd == 9) { /* offset applied here and again in set_period: a PT quirk */
        check_more_effects(r, v);
        set_period(r, v);
    } else {
        set_period(r, v);
    }
}

static void set_dma(replayer_t *r)
{
    paula_write(r, PAULA_DMACON, PAULA_DMACON_SET | r->dmacon_temp);
    for (int v = 0; v < MOD_CHANNELS; v++) {
        write_ptr(r, v, r->ch[v].n_loopstart);
        write_voice(r, v, PAULA_LEN, r->ch[v].n_replen);
    }
}

static uint32_t tick_length(const replayer_t *r)
{
    /* The CIA timer fires on underflow, one count after reaching zero. */
    return (CIA_TIMER_BASE / r->bpm + 1) * CCK_PER_CIA_TICK;
}

static void next_position(replayer_t *r)
{
    r->row = r->pbreak_pos;
    r->pbreak_pos = 0;
    r->pos_jump = 0;
    r->pos = (uint8_t)((r->pos + 1) & 127);
    if (r->pos >= r->song->length) {
        r->pos = 0;
        r->wrapped = 1;
    }
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
    r->low_mask = 0xFF;
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
    if (r->stopped)
        return tick_length(r);
    if (r->cia_set_bpm) {
        r->bpm = r->cia_set_bpm;
        r->cia_set_bpm = 0;
    }
    if (++r->tick >= r->speed) {
        r->tick = 0;
        if (r->patt_del_time2 == 0) {
            r->dmacon_temp = 0;
            r->play_pos = r->pos;
            r->play_row = r->row;
            set_visited(r, r->pos, r->row, 1);
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
        if (r->patt_del_time) {
            r->patt_del_time2 = r->patt_del_time;
            r->patt_del_time = 0;
        }
        if (r->patt_del_time2 && --r->patt_del_time2)
            r->row--; /* pattern delay: repeat this row */
        if (r->pbreak_flag) {
            r->row = r->pbreak_pos;
            r->pbreak_pos = 0;
            r->pbreak_flag = 0;
        }
        if (r->row >= MOD_ROWS || r->pos_jump)
            next_position(r);
    } else {
        for (int v = 0; v < MOD_CHANNELS; v++)
            check_effects(r, v);
        if (r->pos_jump)
            next_position(r);
    }
    /* End of Song: the next tick would read a Row already played. */
    if (r->patt_del_time2 == 0 && r->tick == r->speed - 1 &&
        (visited(r, r->pos, r->row) || r->wrapped)) {
        r->ended = 1;
        r->wrapped = 0;
        for (unsigned i = 0; i < sizeof r->visited; i++) /* start the next pass afresh */
            r->visited[i] = 0;
    }
    return tick_length(r);
}
