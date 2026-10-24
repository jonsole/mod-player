/* pt2_log: prints the Paula writes pt2-clone's replay code makes for a MOD
 * file, in the format shared with replayer_log (host/paula_log.h).
 *
 *   pt2_log song.mod ticks
 *
 * The MOD file is loaded the way pt2-clone loads one (its pt2_load_mod31.c
 * and pt2_module_loader.c, ported here), treating every 4-channel tag as a
 * ProTracker file. Sample pointers are logged as offsets into the Song's
 * sample data as laid out in the file, so they compare directly with ours.
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include "pt2_oracle.h"
#include "paula_log.h"

/* ---- globals the replay code expects ---------------------------------- */
module_t *song;
struct editor_t editor;
struct config_t config = {.maxSampleLength = 65534};
struct ui_t ui;
struct audio_t audio;
struct cursor_t cursor;
const uint8_t vuMeterHeights[65];
const uint32_t tickDuration31fp[(MAX_BPM - MIN_BPM) + 2];

void lockAudio(void) {}
void unlockAudio(void) {}
void resetAudioDither(void) {}
void setLEDFilter(bool state) { (void)state; }
void pointerSetMode(uint8_t mode, bool carry) { (void)mode, (void)carry; }
void pointerSetModeThreadSafe(uint8_t mode, bool carry) { (void)mode, (void)carry; }
void posEdClearNames(void) {}
void renderMuteButtons(void) {}
void updateCurrSample(void) {}
void updateCursorPos(void) {}
void setSyncTickTimeLen(uint32_t timeLen, uint64_t timeLenFrac) { (void)timeLen, (void)timeLenFrac; }
void setVisualsDMACON(uint16_t bits) { (void)bits; }
void setVisualsDataPtr(int32_t ch, const int8_t *src) { (void)ch, (void)src; }
void setVisualsLength(int32_t ch, int32_t len) { (void)ch, (void)len; }
void setVisualsPeriod(int32_t ch, int32_t period) { (void)ch, (void)period; }
void setVisualsVolume(int32_t ch, int32_t vol) { (void)ch, (void)vol; }

/* ---- Paula writes go to the log ----------------------------------------- */
#define NULL_ADDR 0xFFFFFFF0u
static paula_log_t plog;
static int8_t null_sample[0xFFFF * 2];
static uint32_t file_pos[MOD_SAMPLES]; /* where each Sample's data was in the file */

int8_t *paulaGetNullSamplePtr(void) { return null_sample; }

static uint32_t ptr_to_addr(const int8_t *p)
{
    if (p == NULL || (p >= null_sample && p < null_sample + sizeof null_sample))
        return NULL_ADDR;
    ptrdiff_t d = p - song->sampleData;
    int32_t slot = (int32_t)(d / config.maxSampleLength);
    int32_t off = (int32_t)(d % config.maxSampleLength);
    if (d < 0 || slot >= MOD_SAMPLES) {
        fprintf(stderr, "pt2_log: pointer outside Sample memory (slot %d)\n", slot);
        return 0;
    }
    return file_pos[slot] + (uint32_t)off;
}

static int voice_reg(uint32_t address, uint32_t *reg)
{
    if (address == 0xDFF096) {
        *reg = PAULA_DMACON;
        return 1;
    }
    if (address < 0xDFF0A0 || address > 0xDFF0DF)
        return 0;
    static const uint32_t field[16] = {[0] = PAULA_PTR, [4] = PAULA_LEN, [6] = PAULA_PER,
                                       [8] = PAULA_VOL};
    unsigned o = address & 0xF;
    if (o != 0 && o != 4 && o != 6 && o != 8)
        return 0;
    *reg = PAULA_VOICE((address - 0xDFF0A0) >> 4) + field[o];
    return 1;
}

void paulaWriteWord(uint32_t address, uint16_t data16)
{
    uint32_t reg;
    if (voice_reg(address, &reg))
        paula_log_write(&plog, reg, data16);
}

void paulaWritePtr(uint32_t address, const int8_t *ptr)
{
    uint32_t reg;
    if (voice_reg(address, &reg))
        paula_log_write(&plog, reg, ptr_to_addr(ptr));
}

void paulaWriteByte(uint32_t address, uint8_t data8)
{
    if (address == 0xBFE001)
        paula_log_filter(&plog, (data8 >> 1) & 1);
}

/* ---- MOD loading, as pt2-clone does it ---------------------------------- */
static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }

static const char *load(const uint8_t *buf, uint32_t size)
{
    if (size < 1084)
        return "file too small";
    song = createEmptyMod();
    if (!song)
        return "out of memory";
    const uint8_t *p = buf;
    memcpy(song->header.name, p, 20);
    p += 20;

    int32_t real_len[MOD_SAMPLES];
    for (int i = 0; i < MOD_SAMPLES; i++, p += 30) {
        moduleSample_t *s = &song->samples[i];
        memcpy(s->text, p, 22);
        real_len[i] = (int32_t)be16(p + 22) * 2;
        s->length = real_len[i] > config.maxSampleLength ? config.maxSampleLength : real_len[i];
        s->fineTune = p[24] & 0xF;
        s->volume = (int8_t)p[25];
        s->loopStart = (int32_t)be16(p + 26) * 2;
        s->loopLength = (int32_t)be16(p + 28) * 2;
        /* fix for poorly converted STK (< v2.5) -> PT/NT modules */
        if (s->loopLength > 2 && s->loopStart + s->loopLength > s->length &&
            s->loopStart / 2 + s->loopLength <= s->length)
            s->loopStart /= 2;
    }
    song->header.songLength = *p++;
    p++;
    if (song->header.songLength == 129)
        song->header.songLength = 127;
    if (song->header.songLength == 0 || song->header.songLength > 129)
        return "bad song length";
    int num_patterns = 0;
    for (int i = 0; i < 128; i++) {
        song->header.patternTable[i] = *p++;
        if (song->header.patternTable[i] > num_patterns)
            num_patterns = song->header.patternTable[i];
    }
    num_patterns++;
    if (num_patterns > MAX_PATTERNS)
        return "too many patterns";
    p += 4;
    if ((uint32_t)(p - buf) + (uint32_t)num_patterns * 1024 > size)
        return "truncated patterns";
    for (int i = 0; i < num_patterns; i++) {
        note_t *note = song->patterns[i];
        for (int j = 0; j < MOD_ROWS * 4; j++, note++, p += 4) {
            note->period = (uint16_t)(((p[0] & 0x0F) << 8) | p[1]);
            note->sample = ((p[0] & 0xF0) | (p[2] >> 4)) & 31;
            note->command = p[2] & 0x0F;
            note->param = p[3];
        }
    }
    for (int i = 0; i < MOD_SAMPLES; i++) {
        moduleSample_t *s = &song->samples[i];
        file_pos[i] = (uint32_t)(p - buf);
        uint32_t left = size - (uint32_t)(p - buf);
        uint32_t n = (uint32_t)s->length < left ? (uint32_t)s->length : left;
        memcpy(&song->sampleData[s->offset], p, n);
        p += (uint32_t)real_len[i] < left ? (uint32_t)real_len[i] : left;
    }
    /* pt2_module_loader.c's fixes */
    for (int i = 0; i < MOD_SAMPLES; i++) {
        moduleSample_t *s = &song->samples[i];
        if ((uint8_t)s->volume > 64)
            s->volume = 64;
        if (s->loopLength < 2)
            s->loopLength = 2;
        if (s->loopStart > config.maxSampleLength ||
            s->loopStart + s->loopLength > config.maxSampleLength) {
            s->loopStart = 0;
            s->loopLength = 2;
        }
        if (s->length > 0 && s->loopLength > 2 && s->loopStart + s->loopLength > s->length) {
            int32_t over = s->loopStart + s->loopLength - s->length;
            if (s->length + over <= config.maxSampleLength) {
                s->length += over;
            } else {
                s->loopStart = 0;
                s->loopLength = 2;
            }
        }
        if (s->length >= 2 && s->loopStart + s->loopLength <= 2) {
            song->sampleData[s->offset + 0] = 0;
            song->sampleData[s->offset + 1] = 0;
        }
    }
    initializeModuleChannels(song);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: pt2_log song.mod ticks\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "pt2_log: cannot read %s\n", argv[1]);
        return 1;
    }
    static uint8_t buf[0x1000000];
    uint32_t size = (uint32_t)fread(buf, 1, sizeof buf, f);
    fclose(f);
    const char *err = load(buf, size);
    if (err) {
        fprintf(stderr, "pt2_log: %s: %s\n", argv[1], err);
        return 1;
    }

    paula_log_init(&plog, stdout, file_pos[0], NULL_ADDR);
    editor.timingMode = TEMPO_MODE_CIA;
    editor.playMode = PLAY_MODE_NORMAL;
    song->currSpeed = 6;
    song->currBPM = 125;
    modSetSpeed(6);
    modSetTempo(125, false);
    modPlay(DONT_SET_PATTERN, 0, 0);
    paula_log_start(&plog);

    unsigned ticks = (unsigned)atoi(argv[2]);
    for (unsigned t = 1; t <= ticks && editor.songPlaying; t++) {
        tickReplayer();
        uint32_t cck = (uint32_t)lround(AMIGA_PAL_CCK_HZ / ciaBpm2Hz(song->currBPM));
        paula_log_tick(&plog, t, song->currPos, (unsigned)song->currRow, (unsigned)song->currBPM,
                       cck);
    }
    return 0;
}
