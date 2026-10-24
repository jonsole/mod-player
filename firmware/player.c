#include "player.h"
#include "hw.h"
#include "paula_port.h"
#include "replayer.h"
#include "serial.h"

#define PAULA(reg) REG32(PAULA_BASE + (reg))

static mod_song_t song;
static int loaded, playing, paused;
static uint8_t *song_mem;
static uint32_t silence_addr;          /* Paula address of the silence buffer */
static replayer_t rp;

static void paula_write(void *ctx, uint32_t reg, uint32_t value)
{
    (void)ctx;
    PAULA(reg) = value;
}

void player_init(void)
{
    player_stop();
}

const char *player_load(uint8_t *mem, uint32_t size)
{
    loaded = 0;
    mod_error_t err = mod_load(&song, mem, size);
    if (err != MOD_OK)
        return mod_error_text(err);
    song_mem = mem;
    silence_addr = (size + 3) & ~3u;   /* Paula addresses: SDRAM offset */
    volatile uint32_t *z = (volatile uint32_t *)(mem + silence_addr);
    for (uint32_t i = 0; i < PLAYER_SILENCE_BYTES / 4; i++)
        z[i] = 0;
    loaded = 1;
    return 0;
}

const mod_song_t *player_song(void)
{
    return loaded ? &song : 0;
}

static uint32_t options_from_switches(void)
{
    uint32_t sw = SWITCHES, o = 0;
    if (sw & SW_BLEND) o |= PAULA_OPT_BLEND;
    if (sw & SW_A500) o |= PAULA_OPT_A500;
    return o;
}

void player_play(void)
{
    if (!loaded)
        return;
    PAULA(PAULA_CONTROL) = PAULA_CONTROL_FLUSH;    /* stopped, queue emptied */
    PAULA(PAULA_STATUS) = PAULA_STATUS_TICK | PAULA_STATUS_QUEUE_OVERFLOW | PAULA_STATUS_BEHIND;
    PAULA(PAULA_OPTIONS) = options_from_switches();
    paula_port_t port = {paula_write, 0};
    replayer_start(&rp, &song, song_mem, 0, silence_addr, port);
    PAULA(PAULA_TICK_LEN) = replayer_tick(&rp);
    PAULA(PAULA_CONTROL) = PAULA_CONTROL_RUN;      /* first boundary at once */
    playing = 1;
    paused = 0;
}

void player_stop(void)
{
    /* Paula keeps ticking with every voice off: silence. */
    PAULA(PAULA_CONTROL) = PAULA_CONTROL_FLUSH;
    PAULA(PAULA_DMACON) = 0xF;
    for (int v = 0; v < PAULA_VOICES; v++)
        PAULA(PAULA_VOICE(v) + PAULA_VOL) = 0;
    PAULA(PAULA_TICK_LEN) = PLAYER_SILENT_TICK;
    PAULA(PAULA_CONTROL) = PAULA_CONTROL_RUN;
    playing = 0;
    paused = 0;
    LEDS = 0x01;
}

int player_playing(void)
{
    return playing;
}

int player_self_test(void)
{
    /* Stopped, Paula still ticks every PLAYER_SILENT_TICK colour clocks
     * (20 ms): wait for two tick flags, allowing 100 ms. */
    PAULA(PAULA_STATUS) = PAULA_STATUS_TICK;
    uint32_t start = cycles();
    for (int seen = 0; seen < 2;) {
        if (PAULA(PAULA_STATUS) & PAULA_STATUS_TICK) {
            PAULA(PAULA_STATUS) = PAULA_STATUS_TICK;
            seen++;
        }
        if (cycles() - start > CPU_HZ / 10)
            return 0;
    }
    return 1;
}

int player_pause_toggle(void)
{
    if (!playing)
        return 0;
    paused = !paused;
    PAULA(PAULA_CONTROL) = paused ? 0 : PAULA_CONTROL_RUN;
    return paused;
}

/* LEDs: two per voice, a rough level meter from each Channel's volume. */
static void show_levels(void)
{
    uint32_t leds = 0;
    for (int v = 0; v < MOD_CHANNELS; v++) {
        uint8_t vol = rp.ch[v].n_volume;
        leds |= (uint32_t)((vol > 0) | ((vol > 32) << 1)) << (v * 2);
    }
    LEDS = leds;
}

/* Silence at the next tick boundary, like any other tick's writes (ADR-0009). */
static void queue_silence(void)
{
    PAULA(PAULA_DMACON) = 0xF;
    for (int v = 0; v < PAULA_VOICES; v++)
        PAULA(PAULA_VOICE(v) + PAULA_VOL) = 0;
    PAULA(PAULA_TICK_LEN) = PLAYER_SILENT_TICK;
}

void player_poll(void)
{
    if (!playing || paused || !(PAULA(PAULA_STATUS) & PAULA_STATUS_TICK))
        return;
    PAULA(PAULA_STATUS) = PAULA_STATUS_TICK;
    /* The tick just applied stopped the Song (F00), or was its last in
     * play-once mode: let it play, then fall silent. */
    if (rp.stopped || (rp.ended && (SWITCHES & SW_PLAY_ONCE))) {
        queue_silence();
        playing = 0;
        LEDS = 0x01;
        return;
    }
    PAULA(PAULA_TICK_LEN) = replayer_tick(&rp);
    show_levels();
}
