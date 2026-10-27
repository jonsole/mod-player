/* A read-only FAT32 reader for the microSD card (issue #26, ADR-0017):
 * files in the root directory, by long or 8.3 name, case-insensitive. */
#ifndef FAT_H
#define FAT_H

#include <stdint.h>

enum {
    FAT_OK = 0,
    FAT_ERR_CARD,          /* the card didn't start or a read failed: see fat_card_error() */
    FAT_ERR_NOT_FAT32,     /* no FAT32 volume, alone or in an MBR partition */
    FAT_ERR_NOT_FOUND,     /* no file of that name in the root directory */
    FAT_ERR_CHAIN,         /* a cluster chain is damaged: free, bad or out of range, or too short */
    FAT_ERR_TOO_LARGE,     /* the file doesn't fit where it is to go */
};

typedef struct {
    uint32_t first_cluster;
    uint32_t size;
} fat_file_t;

/* Starts the card and finds its FAT32 volume. */
int fat_mount(void);
/* Finds a file in the root directory. name: NUL-terminated; matched against
 * long names and 8.3 names ("NAME.EXT"), ignoring case. */
int fat_find(const char *name, fat_file_t *f);
/* Reads the whole file into dst, which holds max bytes. */
int fat_read(const fat_file_t *f, uint8_t *dst, uint32_t max);
/* Calls fn for each file in the root directory (not directories, volume
 * labels or deleted entries): its long name if it has one, else its 8.3
 * name, and its size. Returns FAT_OK or an error. */
int fat_list(void (*fn)(const char *name, uint32_t size));
/* The Song file after the one whose first cluster is `after` in the root
 * directory, wrapping round to the first: files whose names end in .mod or
 * start with mod. (the Amiga's naming), ignoring case. Its name goes in out
 * (max bytes) and its place in *f. after = 0: the first. */
int fat_next_song(uint32_t after, char *out, int max, fat_file_t *f);
const char *fat_error_text(int err);
/* After FAT_ERR_CARD: the SD driver's error (sd.h). */
int fat_card_error(void);

#endif
