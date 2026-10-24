#include "mod.h"

#define TAG_OFFSET 1080
#define HEADER_BYTES 1084

static unsigned be16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static int tag_is(const uint8_t *tag, const char *s)
{
    return tag[0] == s[0] && tag[1] == s[1] && tag[2] == s[2] && tag[3] == s[3];
}

static int is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static void copy_name(char *dst, const uint8_t *src, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = (char)src[i];
    dst[n] = '\0';
}

static mod_error_t check_tag(const uint8_t *tag)
{
    if (tag_is(tag, "M.K.") || tag_is(tag, "M!K!") || tag_is(tag, "FLT4") ||
        tag_is(tag, "4CHN"))
        return MOD_OK;
    if ((is_digit(tag[0]) && tag[1] == 'C' && tag[2] == 'H' && tag[3] == 'N') ||
        (is_digit(tag[0]) && is_digit(tag[1]) && tag[2] == 'C' && tag[3] == 'H') ||
        tag_is(tag, "FLT8") || tag_is(tag, "OKTA") || tag_is(tag, "CD81") ||
        (tag[0] == 'T' && tag[1] == 'D' && tag[2] == 'Z'))
        return MOD_ERR_NOT_4_CHANNELS;
    return MOD_ERR_NOT_A_MOD;
}

mod_error_t mod_load(mod_song_t *song, uint8_t *data, uint32_t size)
{
    if (size < HEADER_BYTES)
        return MOD_ERR_NOT_A_MOD;
    mod_error_t err = check_tag(data + TAG_OFFSET);
    if (err != MOD_OK)
        return err;

    copy_name(song->name, data, 20);
    song->length = data[950];
    if (song->length == 0 || song->length > 128)
        return MOD_ERR_NOT_A_MOD;

    unsigned max_pattern = 0;
    for (int i = 0; i < 128; i++) {
        song->order[i] = data[952 + i];
        if (song->order[i] > max_pattern)
            max_pattern = song->order[i];
    }
    song->num_patterns = (uint8_t)(max_pattern + 1);
    song->pattern_offset = HEADER_BYTES;

    uint32_t offset = HEADER_BYTES + (uint32_t)song->num_patterns * MOD_PATTERN_BYTES;
    if (offset > size)
        return MOD_ERR_TRUNCATED;

    for (int i = 0; i < MOD_SAMPLES; i++) {
        const uint8_t *h = data + 20 + i * 30;
        mod_sample_t *s = &song->sample[i];
        copy_name(s->name, h, 22);
        uint32_t len = be16(h + 22);
        uint32_t room = (size - offset) / 2;
        if (len > room)
            len = room;
        s->offset = offset;
        s->length = (uint16_t)len;
        s->finetune = h[24] & 0x0F;
        s->volume = h[25] > 64 ? 64 : h[25];
        s->repeat = (uint16_t)be16(h + 26);
        s->replen = (uint16_t)be16(h + 28);
        if (s->replen < 1)
            s->replen = 1;
        if (s->length >= 1 && (uint32_t)s->repeat + s->replen <= 1) {
            data[offset] = 0;
            data[offset + 1] = 0;
        }
        offset += len * 2;
    }
    return MOD_OK;
}

const char *mod_error_text(mod_error_t err)
{
    switch (err) {
    case MOD_OK:
        return "ok";
    case MOD_ERR_NOT_A_MOD:
        return "not a 4-channel MOD file (15-sample Soundtracker files are not supported)";
    case MOD_ERR_NOT_4_CHANNELS:
        return "only 4-channel MOD files are supported";
    case MOD_ERR_TRUNCATED:
        return "file ends in the middle of the Pattern data";
    }
    return "unknown error";
}
