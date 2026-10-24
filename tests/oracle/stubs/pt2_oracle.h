/* Stand-ins for the pt2-clone editor, GUI and audio code that pt2_replayer.c
 * expects. Only what the replay logic touches is real; the rest is inert.
 * Every pt2 header the replayer includes resolves to this file. */
#ifndef PT2_ORACLE_H
#define PT2_ORACLE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(x)
#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#define AMIGA_PAL_XTAL_HZ 28375160
#define AMIGA_PAL_CCK_HZ (AMIGA_PAL_XTAL_HZ / 8.0)
#define CIA_PAL_CLK (AMIGA_PAL_CCK_HZ / 5.0)
#define MOD_ROWS 64
#define MOD_SAMPLES 31
#define MAX_PATTERNS 100
#define PAULA_VOICES 4
#define MIN_BPM 32
#define MAX_BPM 255
#define DONT_SET_PATTERN -1
#define DONT_SET_ORDER -1
#define DO_CARRY true
#define UPDATE_VUMETER 1
#define UPDATE_SPECTRUM_ANALYZER 2

enum { TEMPO_MODE_CIA, TEMPO_MODE_VBLANK };
enum { PLAY_MODE_NORMAL, PLAY_MODE_PATTERN };
enum { MODE_IDLE, MODE_EDIT, MODE_PLAY, MODE_RECORD };
enum { RECORD_PATT, RECORD_SONG };
enum { POINTER_MODE_IDLE, POINTER_MODE_EDIT, POINTER_MODE_PLAY };

typedef struct note_t {
    uint8_t param, sample, command;
    uint16_t period;
} note_t;

typedef struct moduleHeader_t {
    char name[20 + 1];
    uint16_t patternTable[128], songLength;
    uint16_t initialTempo;
} moduleHeader_t;

typedef struct moduleSample_t {
    volatile int8_t *volumeDisp;
    volatile int32_t *lengthDisp, *loopStartDisp, *loopLengthDisp;
    char text[22 + 1];
    int8_t volume;
    uint8_t fineTune;
    int32_t offset, length, loopStart, loopLength;
} moduleSample_t;

typedef struct moduleChannel_t {
    int8_t *n_start, *n_wavestart, *n_loopstart, n_volume, n_dmabit;
    int8_t n_toneportdirec, n_pattpos, n_loopcount;
    uint8_t n_wavecontrol, n_glissfunk, n_sampleoffset, n_toneportspeed;
    uint8_t n_vibratocmd, n_tremolocmd, n_finetune, n_funkoffset, n_samplenum;
    uint8_t n_vibratopos, n_tremolopos;
    int16_t n_period, n_note, n_wantedperiod;
    uint16_t n_cmd, n_length, n_replen;
    uint32_t n_scopedelta, n_chanindex;
    uint8_t syncFlags;
    int8_t syncAnalyzerVolume, syncVuVolume;
    uint16_t syncAnalyzerPeriod;
} moduleChannel_t;

typedef struct module_t {
    bool loaded, modified;
    int8_t *sampleData;
    volatile int32_t tick, speed;
    int8_t row;
    moduleHeader_t header;
    moduleSample_t samples[MOD_SAMPLES];
    moduleChannel_t channels[PAULA_VOICES];
    note_t *patterns[MAX_PATTERNS];
    int8_t currRow;
    int32_t currSpeed, currBPM;
    uint16_t currPos, currPattern;
    uint32_t rowsCounter, rowsInTotal;
} module_t;

extern module_t *song;

extern struct editor_t {
    bool blockMarkFlag, didQuantize, metroFlag, mod2WavOngoing, pat2SmpOngoing, songPlaying;
    bool stepPlayBackwards, stepPlayEnabled, swapChannelFlag, tuningToneFlag, multiModeNext[4];
    bool muted[PAULA_VOICES], rowVisitTable[128 * MOD_ROWS];
    int8_t currMode, playMode, recordMode, stepPlayLastMode, timingMode, hiLowInstr;
    int8_t currSample, editMoveAdd, metroChannel, metroSpeed;
    int16_t f6Pos, f7Pos, f8Pos, f9Pos, f10Pos;
    int32_t initialSpeed, initialTempo, samplePos;
    uint16_t *currEditPatternDisp, *currPatternDisp, *currPosDisp, *currPosEdPattDisp;
    uint32_t playbackSeconds;
    uint64_t playbackSecondsFrac;
    int8_t *sampleZero;
    volatile int8_t vuMeterVolumes[PAULA_VOICES], realVuMeterVolumes[PAULA_VOICES];
    volatile int8_t spectrumVolumes[23];
} editor;

extern struct config_t {
    bool compoMode, enableE8xEffect, keepEditModeAfterStepPlay;
    int32_t maxSampleLength;
} config;

extern struct ui_t {
    bool posEdScreenShown, updateCurrPattText, updatePatternData, updatePosEd, updateSongBPM;
    bool updateSongLength, updateSongName, updateSongPattern, updateSongPos, updateSongSize;
    bool updateStatusText;
} ui;

extern struct audio_t {
    volatile bool locked;
    bool ledFilterEnabled;
    uint32_t samplesPerTickInt, samplesPerTickIntTab[(MAX_BPM - MIN_BPM) + 1];
    uint64_t tickSampleCounter, tickSampleCounterFrac, samplesPerTickFrac;
    uint64_t samplesPerTickFracTab[(MAX_BPM - MIN_BPM) + 1];
    uint32_t tickTimeIntTab[(MAX_BPM - MIN_BPM) + 1];
    uint64_t tickTimeFracTab[(MAX_BPM - MIN_BPM) + 1];
} audio;

extern struct cursor_t {
    uint8_t channel, pos;
} cursor;

extern const uint8_t vuMeterHeights[65];
extern const uint8_t vibratoTable[32];
extern const int16_t periodTable[(37 * 16) + 15];
extern const uint32_t tickDuration31fp[(MAX_BPM - MIN_BPM) + 2];

/* Paula writes: these are what the oracle records. */
void paulaWriteByte(uint32_t address, uint8_t data8);
void paulaWriteWord(uint32_t address, uint16_t data16);
void paulaWritePtr(uint32_t address, const int8_t *ptr);
int8_t *paulaGetNullSamplePtr(void);

/* Inert editor/GUI hooks. */
void lockAudio(void);
void unlockAudio(void);
void resetAudioDither(void);
void setLEDFilter(bool state);
void pointerSetMode(uint8_t mode, bool carry);
void pointerSetModeThreadSafe(uint8_t mode, bool carry);
void posEdClearNames(void);
void renderMuteButtons(void);
void updateCurrSample(void);
void updateCursorPos(void);
void setSyncTickTimeLen(uint32_t timeLen, uint64_t timeLenFrac);
void setVisualsDMACON(uint16_t bits);
void setVisualsDataPtr(int32_t ch, const int8_t *src);
void setVisualsLength(int32_t ch, int32_t len);
void setVisualsPeriod(int32_t ch, int32_t period);
void setVisualsVolume(int32_t ch, int32_t vol);

#include "../pt2-clone/pt2_replayer.h"

#endif
