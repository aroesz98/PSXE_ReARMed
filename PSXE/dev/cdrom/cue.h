#ifndef CUE_H
#define CUE_H

#include "disc.h"

#include <stdint.h>
#include <stddef.h>
#include "ff.h" /* FATFS instead of stdio.h */

enum
{
    CUE_OK = 0,
    CUE_FILE_NOT_FOUND,
    CUE_TRACK_FILE_NOT_FOUND,
    CUE_TRACK_READ_ERROR,
    CUE_BAD_SHEET,   /* no FILE, no TRACK, or more of them than fit */
    CUE_BAD_IMAGE,   /* a bare image that is neither raw nor ISO 9660 */
    CUE_NINTENDO     /* a GameCube or Wii disc, which also come as .iso */
};

/* track modes, as they are spelled in a cue sheet */
enum
{
    CUE_AUDIO = 0,
    CUE_CDG,
    CUE_CDI_2336,
    CUE_CDI_2352,
    CUE_MODE1_2048,
    CUE_MODE1_2352,
    CUE_MODE2_2336,
    CUE_MODE2_2352,
    CUE_NONE = 255
};

enum
{
    LD_BUFFERED, /* (not supported on this target: an image does not fit in RAM) */
    LD_FILE
};

#define CUE_MAX_TRACKS 99
#define CUE_MAX_FILES 99

typedef struct
{
    char *name;           /* path on the card                          */
    uint32_t size;        /* bytes                                     */
    uint32_t data_offset; /* where the sectors begin: a WAVE header    */
    uint32_t start;       /* LBA of its first sector on the disc       */
    uint32_t frames;      /* sectors of the disc it accounts for       */
    uint8_t wave;
} cue_file_t;

typedef struct
{
    int32_t number;
    int32_t mode;

    int32_t index[2];      /* frames into the file, -1: not given             */
    uint32_t pregap;       /* PREGAP: sectors of silence that are in no file  */
    uint32_t sector_bytes; /* 2352, 2336 or 2048                              */
    uint32_t file;         /* which of the files                              */
    uint32_t file_byte;    /* where its first sector is in that file          */
    uint32_t first;        /* LBA of that sector                              */
    uint32_t pre_start;    /* first LBA that counts as this track (its pregap)*/
    uint32_t start;        /* LBA of INDEX 01: what the table of contents says*/
    uint32_t end;          /* one past its last sector                        */
} cue_track_t;

typedef struct
{
    cue_file_t files[CUE_MAX_FILES];
    cue_track_t tracks[CUE_MAX_TRACKS];
    uint32_t file_count;
    uint32_t track_count;

    /* one file is open at a time: a disc can have dozens of audio tracks, each
       in a file of its own, and an open file costs more than half a kilobyte */
    FIL fil;
    int32_t open_file; /* -1: none */

    uint32_t last_track; /* where the last sector was found */
} cue_t;

cue_t *cue_create(void);
void cue_init(cue_t *cue);

/* a cue sheet ... */
int32_t cue_parse(cue_t *cue, const char *path);

/* ... or an image on its own - .bin, .img, .iso - as one data track: raw 2352
   byte sectors or the 2048 byte sectors of a plain ISO 9660 image */
int32_t cue_open_image(cue_t *cue, const char *path);

/* sizes the files and lays the tracks out on the disc */
int32_t cue_load(cue_t *cue, int32_t mode);

// Disc interface
int32_t cue_read(cue_t *cue, uint32_t lba, void *buf);
int32_t cue_query(cue_t *cue, uint32_t lba);
int32_t cue_get_track_number(cue_t *cue, uint32_t lba);
int32_t cue_get_track_count(cue_t *cue);
int32_t cue_get_track_lba(cue_t *cue, int32_t track);
void cue_init_disc(cue_t *cue, psx_disc_t *disc);
void cue_destroy(cue_t *cue);

#endif
