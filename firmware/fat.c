/* FAT32, read-only (see fat.h). The layout is Microsoft's FAT specification:
 * a volume starts with a boot sector whose BIOS parameter block gives the
 * sizes; then the reserved sectors, the FATs, and the data area of clusters
 * numbered from 2. A file is a chain of clusters through the FAT (28-bit
 * entries; 0x0FFFFFF8 and above end it). The root directory is a chain too,
 * of 32-byte entries; long names are stored in entries before the 8.3 one,
 * last part first, 13 UTF-16 characters each. */
#include "fat.h"

#include "sd.h"

#define EOC_MIN 0x0FFFFFF8u
#define ATTR_LFN 0x0Fu
#define ATTR_VOLUME 0x08u
#define ATTR_DIR 0x10u
#define NAME_MAX 255

static uint8_t sec[SD_BLOCK];          /* the sector being looked at */
static uint8_t fat_sec[SD_BLOCK];      /* the FAT sector last read */
static uint32_t fat_sec_lba = 0xFFFFFFFFu;
static uint32_t fat_start, data_start, root_cluster, clusters, sectors_per_cluster;
static int mounted, card_err;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_sector(uint32_t lba, uint8_t *dst)
{
    int err = sd_read_block(lba, dst);
    if (err) {
        card_err = err;
        return FAT_ERR_CARD;
    }
    return FAT_OK;
}

/* Is sec a FAT32 boot sector? If so, take the volume's layout from it. */
static int take_boot_sector(uint32_t volume)
{
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return 0;
    uint32_t bytes = u16(sec + 11), spc = sec[13], reserved = u16(sec + 14), fats = sec[16];
    uint32_t root_entries = u16(sec + 17), fat16_size = u16(sec + 22);
    uint32_t total = u16(sec + 19) ? u16(sec + 19) : u32(sec + 32), fat_size = u32(sec + 36);
    if (bytes != SD_BLOCK || spc == 0 || (spc & (spc - 1)) || reserved == 0 || fats == 0 ||
        root_entries != 0 || fat16_size != 0 || fat_size == 0)
        return 0;                                   /* not FAT32 (FAT12/16 have root entries, a FAT16 size) */
    uint32_t data = reserved + fats * fat_size;
    if (total <= data)
        return 0;
    fat_start = volume + reserved;
    data_start = volume + data;
    sectors_per_cluster = spc;
    clusters = (total - data) / spc;
    root_cluster = u32(sec + 44);
    if (clusters == 0 || root_cluster < 2 || root_cluster >= clusters + 2)
        return 0;
    return 1;
}

int fat_mount(void)
{
    mounted = 0;
    fat_sec_lba = 0xFFFFFFFFu;
    int err = sd_init();
    if (err) {
        card_err = err;
        return FAT_ERR_CARD;
    }
    if ((err = read_sector(0, sec)))
        return err;
    if (take_boot_sector(0)) {                      /* a volume without a partition table */
        mounted = 1;
        return FAT_OK;
    }
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return FAT_ERR_NOT_FAT32;
    uint32_t starts[4];
    int n = 0;
    for (int i = 0; i < 4; i++) {                   /* an MBR: the FAT32 partitions */
        const uint8_t *p = sec + 446 + 16 * i;
        uint8_t type = p[4];
        if ((type == 0x0B || type == 0x0C || type == 0x1B || type == 0x1C) && u32(p + 8) != 0)
            starts[n++] = u32(p + 8);
    }
    for (int i = 0; i < n; i++) {
        if ((err = read_sector(starts[i], sec)))
            return err;
        if (take_boot_sector(starts[i])) {
            mounted = 1;
            return FAT_OK;
        }
    }
    return FAT_ERR_NOT_FAT32;
}

/* The cluster after c in its chain, or an error. */
static int next_cluster(uint32_t c, uint32_t *next)
{
    uint32_t lba = fat_start + c / (SD_BLOCK / 4);
    if (lba != fat_sec_lba) {
        int err = read_sector(lba, fat_sec);
        if (err)
            return err;
        fat_sec_lba = lba;
    }
    *next = u32(fat_sec + (c % (SD_BLOCK / 4)) * 4) & 0x0FFFFFFFu;
    return FAT_OK;
}

static int valid_cluster(uint32_t c)
{
    return c >= 2 && c < clusters + 2;
}

static uint32_t cluster_lba(uint32_t c)
{
    return data_start + (c - 2) * sectors_per_cluster;
}

static char upper(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

static int same_name(const char *a, const char *b)
{
    while (*a && upper(*a) == upper(*b))
        a++, b++;
    return *a == 0 && *b == 0;
}

static uint8_t lfn_checksum(const uint8_t *short11)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + short11[i]);
    return s;
}

/* Walks the root directory, calling fn for each file; fn returns nonzero to
 * stop. The long name being collected is kept here between entries. */
typedef int (*visit_fn)(const char *name, uint32_t first, uint32_t size, void *arg);

static int walk_root(visit_fn fn, void *arg)
{
    if (!mounted)
        return FAT_ERR_NOT_FAT32;
    static char lfn[NAME_MAX + 1];
    char shortname[13];
    int lfn_ok = 0;
    uint8_t lfn_sum = 0;
    uint32_t c = root_cluster, seen = 0;
    for (;;) {
        if (!valid_cluster(c) || ++seen > clusters)     /* out of range, or a loop */
            return FAT_ERR_CHAIN;
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            int err = read_sector(cluster_lba(c) + s, sec);
            if (err)
                return err;
            for (uint32_t i = 0; i < SD_BLOCK; i += 32) {
                const uint8_t *e = sec + i;
                uint8_t attr = e[11];
                if (e[0] == 0x00)
                    return FAT_OK;                          /* the end of the directory */
                if (e[0] == 0xE5) {                         /* deleted */
                    lfn_ok = 0;
                    continue;
                }
                if ((attr & 0x3F) == ATTR_LFN) {
                    int seq = e[0] & 0x1F;
                    if (seq == 0 || seq > 20) {
                        lfn_ok = 0;
                        continue;
                    }
                    if (e[0] & 0x40) {                      /* the last part comes first */
                        lfn_ok = 1;
                        lfn_sum = e[13];
                        int end = seq * 13 < NAME_MAX ? seq * 13 : NAME_MAX;
                        lfn[end] = 0;
                    } else if (!lfn_ok || e[13] != lfn_sum) {
                        lfn_ok = 0;
                        continue;
                    }
                    static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (int k = 0; k < 13; k++) {
                        int pos = (seq - 1) * 13 + k;
                        uint16_t ch = u16(e + at[k]);
                        if (pos >= NAME_MAX)
                            break;
                        if (ch == 0x0000) {
                            lfn[pos] = 0;
                            break;
                        }
                        if (ch == 0xFFFF)
                            break;
                        lfn[pos] = ch < 0x80 ? (char)ch : '?';   /* only ASCII is matched */
                    }
                    continue;
                }
                int have_lfn = lfn_ok && lfn_sum == lfn_checksum(e);
                lfn_ok = 0;
                if (attr & (ATTR_VOLUME | ATTR_DIR))
                    continue;
                /* the 8.3 name as NAME.EXT */
                int n = 0;
                for (int k = 0; k < 8 && e[k] != ' '; k++)
                    shortname[n++] = (char)(k == 0 && e[0] == 0x05 ? 0xE5 : e[k]);
                if (e[8] != ' ') {
                    shortname[n++] = '.';
                    for (int k = 8; k < 11 && e[k] != ' '; k++)
                        shortname[n++] = (char)e[k];
                }
                shortname[n] = 0;
                uint32_t first = (uint32_t)u16(e + 20) << 16 | u16(e + 26);
                int stop = fn(have_lfn ? lfn : shortname, first, u32(e + 28), arg);
                if (!stop && have_lfn)
                    stop = fn(shortname, first, u32(e + 28), arg);   /* the alias matches too */
                if (stop)
                    return FAT_OK;
            }
        }
        uint32_t next;
        int err = next_cluster(c, &next);
        if (err)
            return err;
        if (next >= EOC_MIN)
            return FAT_OK;
        c = next;
    }
}

struct find_arg {
    const char *name;
    fat_file_t *f;
    int found;
};

static int find_visit(const char *name, uint32_t first, uint32_t size, void *arg)
{
    struct find_arg *a = arg;
    if (!same_name(a->name, name))
        return 0;
    a->f->first_cluster = first;
    a->f->size = size;
    a->found = 1;
    return 1;
}

int fat_find(const char *name, fat_file_t *f)
{
    struct find_arg a = {name, f, 0};
    int err = walk_root(find_visit, &a);
    if (err)
        return err;
    return a.found ? FAT_OK : FAT_ERR_NOT_FOUND;
}

int fat_read(const fat_file_t *f, uint8_t *dst, uint32_t max)
{
    if (!mounted)
        return FAT_ERR_NOT_FAT32;
    if (f->size > max)
        return FAT_ERR_TOO_LARGE;
    uint32_t done = 0, c = f->first_cluster, used = 0;
    while (done < f->size) {
        if (!valid_cluster(c) || ++used > clusters)
            return FAT_ERR_CHAIN;
        for (uint32_t s = 0; s < sectors_per_cluster && done < f->size; s++) {
            uint32_t left = f->size - done;
            if (left >= SD_BLOCK) {                         /* a whole sector, straight to dst */
                int err = read_sector(cluster_lba(c) + s, dst + done);
                if (err)
                    return err;
                done += SD_BLOCK;
            } else {                                        /* the last part of one */
                int err = read_sector(cluster_lba(c) + s, sec);
                if (err)
                    return err;
                for (uint32_t k = 0; k < left; k++)
                    dst[done + k] = sec[k];
                done += left;
            }
        }
        if (done < f->size) {
            uint32_t next;
            int err = next_cluster(c, &next);
            if (err)
                return err;
            if (next >= EOC_MIN)                            /* the chain ends before the file */
                return FAT_ERR_CHAIN;
            c = next;
        }
    }
    return FAT_OK;
}

struct list_arg {
    void (*fn)(const char *name, uint32_t size);
    uint32_t last_first;
    int have_last;
};

static int list_visit(const char *name, uint32_t first, uint32_t size, void *arg)
{
    struct list_arg *a = arg;
    if (a->have_last && first == a->last_first && first != 0)
        return 0;                                           /* the 8.3 alias of the file just listed */
    a->last_first = first;
    a->have_last = 1;
    a->fn(name, size);
    return 0;
}

int fat_list(void (*fn)(const char *name, uint32_t size))
{
    struct list_arg a = {fn, 0, 0};
    return walk_root(list_visit, &a);
}

static int song_name(const char *name)
{
    int n = 0;
    while (name[n])
        n++;
    if (n >= 4 && same_name(".mod", name + n - 4))
        return 1;
    return n > 4 && upper(name[0]) == 'M' && upper(name[1]) == 'O' && upper(name[2]) == 'D' && name[3] == '.';
}

struct next_arg {
    uint32_t after, last_first;
    int have_last, seen_after, have_first, got, max;
    char *out;
    fat_file_t *f, first;
    char first_name[NAME_MAX + 1];
};

static void copy_name(char *dst, const char *src, int max)
{
    int i = 0;
    for (; src[i] && i < max - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static int next_visit(const char *name, uint32_t first, uint32_t size, void *arg)
{
    struct next_arg *a = arg;
    if (a->have_last && first == a->last_first && first != 0)
        return 0;                                           /* the alias of the file just seen */
    a->last_first = first;
    a->have_last = 1;
    int song = song_name(name) && size > 0;
    if (song && a->seen_after) {
        copy_name(a->out, name, a->max);
        a->f->first_cluster = first;
        a->f->size = size;
        a->got = 1;
        return 1;
    }
    if (song && !a->have_first) {
        copy_name(a->first_name, name, sizeof a->first_name);
        a->first.first_cluster = first;
        a->first.size = size;
        a->have_first = 1;
    }
    if (first == a->after && a->after != 0)
        a->seen_after = 1;
    return 0;
}

int fat_next_song(uint32_t after, char *out, int max, fat_file_t *f)
{
    static struct next_arg a;
    a.after = after;
    a.have_last = a.seen_after = a.have_first = a.got = 0;
    a.out = out;
    a.max = max;
    a.f = f;
    int err = walk_root(next_visit, &a);
    if (err)
        return err;
    if (a.got)
        return FAT_OK;
    if (!a.have_first)
        return FAT_ERR_NOT_FOUND;
    copy_name(out, a.first_name, max);                      /* wrap round to the first */
    *f = a.first;
    return FAT_OK;
}

int fat_card_error(void)
{
    return card_err;
}

const char *fat_error_text(int err)
{
    switch (err) {
    case FAT_ERR_CARD: return sd_error_text(card_err);
    case FAT_ERR_NOT_FAT32: return "card not FAT32";
    case FAT_ERR_NOT_FOUND: return "file not found";
    case FAT_ERR_CHAIN: return "damaged FAT chain";
    case FAT_ERR_TOO_LARGE: return "file too large";
    default: return "FAT error";
    }
}
