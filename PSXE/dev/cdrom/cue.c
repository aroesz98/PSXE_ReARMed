/*
    Disc images: a cue sheet with its track files, or an image on its own.

    What this gives the CD-ROM emulation is a disc addressed by LBA (sector 0 is
    00:00:00, so the first track starts at 150): which track a sector belongs to,
    whether it is data, audio or pregap, and its 2352 raw bytes.

    Layout rules, which is where images differ:

      - Every FILE of a sheet follows the previous one on the disc. One track per
        file with "INDEX 00 00:00:00 / INDEX 01 00:02:00" is how a multi track
        disc is usually dumped: the two seconds of pregap are in the file, the
        track - what the table of contents points at - starts at INDEX 01.
      - Several tracks in one file: the INDEX times are positions in that file.
      - PREGAP is silence that is in no file; it pushes everything after it back.
      - Track files are raw 2352 byte sectors. MODE1/2048 and MODE2/2336 tracks,
        and a plain .iso, hold less than that, and the rest of the raw sector is
        made up when it is read: sync, address, mode 2 and a form 1 subheader,
        which is what a PlayStation expects to find around its data.
*/

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "cue.h"

/* FATFS includes */
#include "ff.h"
#include "fsl_debug_console.h"

#define CUE_RAW_BYTES 2352u
#define CUE_TEXT_MAX (64u * 1024u) /* a cue sheet is text, a few kilobytes at most */
#define CUE_FIRST_LBA 150u         /* 00:02:00 */

#ifndef CUE_SDRAM
#define CUE_SDRAM __attribute__((section(".bss.$BOARD_SDRAM"), aligned(64)))
#endif

static cue_t CUE_SDRAM sCue;

/*
    Read ahead for disc images on the SD card.

    A CD sector is 2352 bytes, which is no multiple of the card's 512: read one
    at a time, every sector turned into three card transactions - the tail of one
    block through the file's sector buffer, a few whole blocks, the head of the
    next - and their latency, not the amount of data, was what reading cost. A
    drive reads sequentially almost all of the time, so the image is read in
    block aligned windows instead: one transaction serves about fourteen sectors,
    data and the XA audio interleaved with it alike.

    The window is its own cache line aligned buffer because the card driver runs
    its DMA into it and invalidates the data cache over exactly that range.
*/
#define CUE_RA_BLOCK 512u
#define CUE_RA_SIZE (64u * CUE_RA_BLOCK)

static uint8_t CUE_SDRAM g_cue_ra_buf[CUE_RA_SIZE];
static FIL *g_cue_ra_file = NULL; /* the image the window belongs to */
static uint32_t g_cue_ra_pos = 0; /* file offset of its first byte   */
static uint32_t g_cue_ra_len = 0; /* valid bytes in it               */

static void cue_ra_drop(void)
{
    g_cue_ra_file = NULL;
    g_cue_ra_len = 0;
}

/* `left` bytes at `offset` of an image file, through the window */
static void cue_ra_read(FIL *fp, uint32_t offset, uint8_t *dst, uint32_t left)
{
    while (left)
    {
        if ((fp != g_cue_ra_file) || (offset < g_cue_ra_pos) || (offset >= (g_cue_ra_pos + g_cue_ra_len)))
        {
            UINT got = 0;

            g_cue_ra_file = fp;
            g_cue_ra_pos = offset & ~(CUE_RA_BLOCK - 1u);
            g_cue_ra_len = 0;

            if ((f_lseek(fp, g_cue_ra_pos) != FR_OK) ||
                (f_read(fp, g_cue_ra_buf, CUE_RA_SIZE, &got) != FR_OK) || (got == 0) ||
                (offset >= (g_cue_ra_pos + got)))
            {
                /* past the end of the image, or a card error: reads as zeroes,
                   like the short read this replaces */
                cue_ra_drop();
                memset(dst, 0, left);

                return;
            }

            g_cue_ra_len = got;
        }

        const uint32_t at = offset - g_cue_ra_pos;

        uint32_t n = g_cue_ra_len - at;

        if (n > left)
            n = left;

        memcpy(dst, &g_cue_ra_buf[at], n);

        dst += n;
        offset += n;
        left -= n;
    }
}

/* what is beyond 4 GB of a file is of no use to a CD */
static inline uint32_t cue_size_of(FIL *fp)
{
    const FSIZE_t size = f_size(fp);

    return (size > (FSIZE_t)0xffffffffu) ? 0xffffffffu : (uint32_t)size;
}

/* ------------------------------------------------------------------ the sheet */

static const struct
{
    const char *word;
    int32_t mode;
    uint32_t bytes;
} cue_modes[] = {
    {"AUDIO", CUE_AUDIO, 2352u},
    {"CDG", CUE_CDG, 2352u},
    {"CDI/2336", CUE_CDI_2336, 2336u},
    {"CDI/2352", CUE_CDI_2352, 2352u},
    {"MODE1/2048", CUE_MODE1_2048, 2048u},
    {"MODE1/2352", CUE_MODE1_2352, 2352u},
    {"MODE2/2336", CUE_MODE2_2336, 2336u},
    {"MODE2/2352", CUE_MODE2_2352, 2352u},
};

static int cue_word_is(const char *word, uint32_t len, const char *what)
{
    uint32_t i = 0;

    for (; (i < len) && what[i]; i++)
    {
        if (toupper((unsigned char)word[i]) != what[i])
            return 0;
    }

    return (i == len) && !what[i];
}

/* mm:ss:ff at *p, as frames; -1 when that is not what is there */
static int32_t cue_parse_msf(const char *p, const char *end)
{
    uint32_t v[3] = {0, 0, 0};

    for (int k = 0; k < 3; k++)
    {
        if ((p >= end) || !isdigit((unsigned char)*p))
            return -1;

        while ((p < end) && isdigit((unsigned char)*p))
            v[k] = (v[k] * 10u) + (uint32_t)(*p++ - '0');

        if (k < 2)
        {
            if ((p >= end) || (*p != ':'))
                return -1;

            p++;
        }
    }

    return (int32_t)((v[0] * 4500u) + (v[1] * 75u) + v[2]);
}

static char *cue_join_path(const char *sheet_path, const char *name, uint32_t name_len)
{
    /* the directory of the sheet ... */
    uint32_t dir_len = 0;

    for (uint32_t i = 0; sheet_path[i]; i++)
    {
        if ((sheet_path[i] == '/') || (sheet_path[i] == '\\'))
            dir_len = i + 1u;
    }

    /* ... and the file's own name: whatever directory a sheet names is where the
       image was made, not where it is now */
    for (uint32_t i = 0; i < name_len; i++)
    {
        if ((name[i] == '/') || (name[i] == '\\'))
        {
            name += i + 1u;
            name_len -= i + 1u;
            i = (uint32_t)-1;
        }
    }

    char *path = malloc(dir_len + name_len + 1u);

    if (!path)
        return NULL;

    memcpy(path, sheet_path, dir_len);
    memcpy(path + dir_len, name, name_len);
    path[dir_len + name_len] = '\0';

    return path;
}

static void cue_forget(cue_t *cue)
{
    for (uint32_t i = 0; i < cue->file_count; i++)
        free(cue->files[i].name);

    cue->file_count = 0;
    cue->track_count = 0;
    cue->last_track = 0;
}

/* one line of a sheet, [p, end) without the line ending */
static int32_t cue_parse_line(cue_t *cue, const char *sheet_path, const char *p, const char *end)
{
    while ((p < end) && isspace((unsigned char)*p))
        p++;

    const char *word = p;

    while ((p < end) && !isspace((unsigned char)*p))
        p++;

    const uint32_t word_len = (uint32_t)(p - word);

    while ((p < end) && isspace((unsigned char)*p))
        p++;

    if (cue_word_is(word, word_len, "FILE"))
    {
        /* FILE "name with spaces.bin" BINARY  -  or without the quotes */
        const char *name;
        const char *name_end;

        if ((p < end) && (*p == '"'))
        {
            name = ++p;

            while ((p < end) && (*p != '"'))
                p++;

            name_end = p;

            if (p < end)
                p++;
        }
        else
        {
            name = p;
            name_end = end;

            /* the last word is the type */
            while ((name_end > name) && isspace((unsigned char)name_end[-1]))
                name_end--;

            const char *q = name_end;

            while ((q > name) && !isspace((unsigned char)q[-1]))
                q--;

            if (q > name)
            {
                p = q;
                name_end = q;

                while ((name_end > name) && isspace((unsigned char)name_end[-1]))
                    name_end--;
            }
            else
            {
                p = end;
            }
        }

        if ((name_end <= name) || (cue->file_count >= CUE_MAX_FILES))
            return CUE_BAD_SHEET;

        while ((p < end) && isspace((unsigned char)*p))
            p++;

        const char *type = p;

        while ((p < end) && !isspace((unsigned char)*p))
            p++;

        cue_file_t *file = &cue->files[cue->file_count];

        memset(file, 0, sizeof(*file));

        file->name = cue_join_path(sheet_path, name, (uint32_t)(name_end - name));
        file->wave = (uint8_t)cue_word_is(type, (uint32_t)(p - type), "WAVE");

        if (!file->name)
            return CUE_BAD_SHEET;

        cue->file_count++;

        return CUE_OK;
    }

    if (cue_word_is(word, word_len, "TRACK"))
    {
        if (!cue->file_count || (cue->track_count >= CUE_MAX_TRACKS))
            return CUE_BAD_SHEET;

        cue_track_t *track = &cue->tracks[cue->track_count];

        memset(track, 0, sizeof(*track));

        track->index[0] = -1;
        track->index[1] = -1;
        track->file = cue->file_count - 1u;
        track->mode = CUE_NONE;
        track->sector_bytes = CUE_RAW_BYTES;

        while ((p < end) && isdigit((unsigned char)*p))
            track->number = (track->number * 10) + (*p++ - '0');

        while ((p < end) && isspace((unsigned char)*p))
            p++;

        const char *mode = p;

        while ((p < end) && !isspace((unsigned char)*p))
            p++;

        for (uint32_t i = 0; i < (sizeof(cue_modes) / sizeof(cue_modes[0])); i++)
        {
            if (cue_word_is(mode, (uint32_t)(p - mode), cue_modes[i].word))
            {
                track->mode = cue_modes[i].mode;
                track->sector_bytes = cue_modes[i].bytes;
            }
        }

        if (track->mode == CUE_NONE)
        {
            PRINTF("cue: track %d has a mode this does not know, taken as raw data\r\n", (int)track->number);

            track->mode = CUE_MODE2_2352;
        }

        cue->track_count++;

        return CUE_OK;
    }

    if (!cue->track_count)
        return CUE_OK; /* REM, CATALOG, TITLE ... ahead of the first track */

    cue_track_t *track = &cue->tracks[cue->track_count - 1u];

    if (cue_word_is(word, word_len, "INDEX"))
    {
        uint32_t n = 0;

        while ((p < end) && isdigit((unsigned char)*p))
            n = (n * 10u) + (uint32_t)(*p++ - '0');

        while ((p < end) && isspace((unsigned char)*p))
            p++;

        /* only the pregap and the start of a track matter here */
        if (n <= 1u)
            track->index[n] = cue_parse_msf(p, end);
    }
    else if (cue_word_is(word, word_len, "PREGAP"))
    {
        const int32_t frames = cue_parse_msf(p, end);

        if (frames > 0)
            track->pregap = (uint32_t)frames;
    }

    /* everything else - FLAGS, ISRC, TITLE, PERFORMER, POSTGAP, REM - changes
       nothing about where a sector is */
    return CUE_OK;
}

cue_t *cue_create(void)
{
    return &sCue;
}

void cue_init(cue_t *cue)
{
    memset(cue, 0, sizeof(*cue));

    cue->open_file = -1;
}

int32_t cue_parse(cue_t *cue, const char *path)
{
    FIL *const fp = &cue->fil;

    if (f_open(fp, path, FA_READ) != FR_OK)
        return CUE_FILE_NOT_FOUND;

    uint32_t size = cue_size_of(fp);

    if (size > CUE_TEXT_MAX)
        size = CUE_TEXT_MAX;

    char *text = malloc(size + 1u);

    UINT got = 0;

    if (!text || (f_read(fp, text, size, &got) != FR_OK))
    {
        free(text);
        f_close(fp);

        return CUE_FILE_NOT_FOUND;
    }

    f_close(fp);

    text[got] = '\0';

    int32_t result = CUE_OK;

    const char *p = text;
    const char *const text_end = text + got;

    /* a byte order mark is not part of the first keyword */
    if ((got >= 3u) && ((uint8_t)p[0] == 0xefu) && ((uint8_t)p[1] == 0xbbu) && ((uint8_t)p[2] == 0xbfu))
        p += 3;

    while ((p < text_end) && (result == CUE_OK))
    {
        const char *line = p;

        while ((p < text_end) && (*p != '\n') && (*p != '\r'))
            p++;

        result = cue_parse_line(cue, path, line, p);

        while ((p < text_end) && ((*p == '\n') || (*p == '\r')))
            p++;
    }

    free(text);

    if ((result == CUE_OK) && (!cue->file_count || !cue->track_count))
        result = CUE_BAD_SHEET;

    if (result != CUE_OK)
        cue_forget(cue);

    return result;
}

/* ------------------------------------------------------------ a bare image */

int32_t cue_open_image(cue_t *cue, const char *path)
{
    static const uint8_t sync[12] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

    FIL *const fp = &cue->fil;

    if (f_open(fp, path, FA_READ) != FR_OK)
        return CUE_FILE_NOT_FOUND;

    const uint32_t size = cue_size_of(fp);

    uint8_t head[32];
    uint8_t pvd[8];
    UINT got_head = 0, got_pvd = 0;

    memset(head, 0, sizeof(head));
    memset(pvd, 0, sizeof(pvd));

    /* sector 16 of an image of 2048 byte sectors is the volume descriptor */
    const int ok = (f_read(fp, head, sizeof(head), &got_head) == FR_OK) && (f_lseek(fp, 16u * 2048u) == FR_OK) &&
                   (f_read(fp, pvd, sizeof(pvd), &got_pvd) == FR_OK);

    f_close(fp);

    if (!ok)
        return CUE_TRACK_READ_ERROR;

    /* An .iso is whatever somebody called one. GameCube and Wii images are about
       as common as PlayStation ones and say what they are in their first sector. */
    {
        static const uint8_t gamecube[4] = {0xc2, 0x33, 0x9f, 0x3d}; /* at 0x1c */
        static const uint8_t wii[4] = {0x5d, 0x1c, 0x9e, 0xa3};      /* at 0x18 */

        if ((got_head == sizeof(head)) && (!memcmp(&head[0x1c], gamecube, 4) || !memcmp(&head[0x18], wii, 4)))
            return CUE_NINTENDO;
    }

    uint32_t bytes;

    if ((got_head == sizeof(head)) && !memcmp(head, sync, sizeof(sync)))
        bytes = CUE_RAW_BYTES;
    else if ((got_pvd == sizeof(pvd)) && (pvd[0] == 1u) && !memcmp(&pvd[1], "CD001", 5))
        bytes = 2048u;
    else
        return CUE_BAD_IMAGE;

    cue_file_t *file = &cue->files[0];

    memset(file, 0, sizeof(*file));

    file->name = malloc(strlen(path) + 1u);

    if (!file->name)
        return CUE_BAD_IMAGE;

    strcpy(file->name, path);

    cue_track_t *track = &cue->tracks[0];

    memset(track, 0, sizeof(*track));

    track->number = 1;
    track->mode = (bytes == CUE_RAW_BYTES) ? CUE_MODE2_2352 : CUE_MODE1_2048;
    track->sector_bytes = bytes;
    track->index[0] = -1;
    track->index[1] = 0;

    cue->file_count = 1;
    cue->track_count = 1;

    PRINTF("image: %s, %u MB of %u byte sectors\r\n", path, (unsigned)(size >> 20), (unsigned)bytes);

    return CUE_OK;
}

/* ------------------------------------------------------------------ layout */

/* where the samples begin in a WAVE file */
static uint32_t cue_wave_data_offset(FIL *fp)
{
    uint8_t head[256];
    UINT got = 0;

    if ((f_lseek(fp, 0) != FR_OK) || (f_read(fp, head, sizeof(head), &got) != FR_OK))
        return 44u;

    for (uint32_t i = 12u; (i + 8u) <= got; i++)
    {
        if (!memcmp(&head[i], "data", 4))
            return i + 8u;
    }

    return 44u;
}

int32_t cue_load(cue_t *cue, int32_t mode)
{
    (void)mode;

    for (uint32_t i = 0; i < cue->file_count; i++)
    {
        cue_file_t *file = &cue->files[i];

        if (f_open(&cue->fil, file->name, FA_READ) != FR_OK)
        {
            PRINTF("cue: cannot open '%s'\r\n", file->name);

            return CUE_TRACK_FILE_NOT_FOUND;
        }

        file->size = cue_size_of(&cue->fil);
        file->data_offset = file->wave ? cue_wave_data_offset(&cue->fil) : 0u;

        f_close(&cue->fil);
    }

    uint32_t lba = CUE_FIRST_LBA;
    uint32_t t = 0;

    for (uint32_t i = 0; i < cue->file_count; i++)
    {
        cue_file_t *file = &cue->files[i];

        file->start = lba;

        /* the position in the file as the tracks are walked: frames by the
           sheet's clock, and the bytes that is - tracks of one file need not
           have sectors of one size */
        uint32_t at_frame = 0;
        uint32_t at_byte = file->data_offset;
        uint32_t frame_bytes = 0;
        uint32_t shift = 0; /* PREGAP sectors so far: on the disc, not in the file */

        cue_track_t *prev = NULL;

        for (; (t < cue->track_count) && (cue->tracks[t].file == i); t++)
        {
            cue_track_t *track = &cue->tracks[t];

            if (track->index[1] < 0)
                track->index[1] = (track->index[0] >= 0) ? track->index[0] : 0;

            if ((track->index[0] < 0) || (track->index[0] > track->index[1]))
                track->index[0] = -1;

            uint32_t first = (uint32_t)((track->index[0] >= 0) ? track->index[0] : track->index[1]);

            if (first < at_frame)
                first = at_frame; /* a sheet that goes backwards */

            if (!frame_bytes)
                frame_bytes = track->sector_bytes;

            at_byte += (first - at_frame) * frame_bytes;
            at_frame = first;
            frame_bytes = track->sector_bytes;

            shift += track->pregap;

            track->file_byte = at_byte;
            track->first = file->start + first + shift;
            track->pre_start = track->first - track->pregap;
            track->start = track->first + ((uint32_t)track->index[1] - first);

            if (prev)
                prev->end = track->pre_start;

            prev = track;
        }

        if (prev)
        {
            const uint32_t left = (file->size > at_byte) ? ((file->size - at_byte) / frame_bytes) : 0u;

            prev->end = prev->first + left;

            if (prev->end < prev->start)
                prev->end = prev->start;

            lba = prev->end;
        }

        file->frames = lba - file->start;

        PRINTF("cue: '%s' %u sectors from %u\r\n", file->name, (unsigned)file->frames, (unsigned)file->start);
    }

    for (uint32_t i = 0; i < cue->track_count; i++)
    {
        const cue_track_t *track = &cue->tracks[i];

        PRINTF("cue: track %02d %s %02u:%02u:%02u - %02u:%02u:%02u\r\n", (int)track->number,
               (track->mode == CUE_AUDIO) ? "audio" : "data ",
               (unsigned)(track->start / 4500u), (unsigned)((track->start / 75u) % 60u), (unsigned)(track->start % 75u),
               (unsigned)(track->end / 4500u), (unsigned)((track->end / 75u) % 60u), (unsigned)(track->end % 75u));
    }

    return CUE_OK;
}

void cue_destroy(cue_t *cue)
{
    if (cue->open_file >= 0)
    {
        /* the window may still hold this image's data */
        cue_ra_drop();
        f_close(&cue->fil);

        cue->open_file = -1;
    }

    cue_forget(cue);
}

/* ------------------------------------------------------------------ reading */

static cue_track_t *cue_track_at(cue_t *cue, uint32_t lba)
{
    if (cue->last_track < cue->track_count)
    {
        cue_track_t *track = &cue->tracks[cue->last_track];

        if ((lba >= track->pre_start) && (lba < track->end))
            return track;
    }

    for (uint32_t i = 0; i < cue->track_count; i++)
    {
        cue_track_t *track = &cue->tracks[i];

        if ((lba >= track->pre_start) && (lba < track->end))
        {
            cue->last_track = i;

            return track;
        }
    }

    return NULL;
}

static inline uint32_t cue_lead_out(const cue_t *cue)
{
    return cue->track_count ? cue->tracks[cue->track_count - 1u].end : 0u;
}

static inline int32_t cue_kind(const cue_track_t *track, uint32_t lba)
{
    if (lba < track->start)
        return TS_PREGAP;

    return (track->mode == CUE_AUDIO) ? TS_AUDIO : TS_DATA;
}

static FIL *cue_file_handle(cue_t *cue, uint32_t file)
{
    if (cue->open_file == (int32_t)file)
        return &cue->fil;

    if (cue->open_file >= 0)
    {
        cue_ra_drop();
        f_close(&cue->fil);

        cue->open_file = -1;
    }

    if (f_open(&cue->fil, cue->files[file].name, FA_READ) != FR_OK)
        return NULL;

    cue->open_file = (int32_t)file;

    return &cue->fil;
}

static inline uint8_t cue_bcd(uint32_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* what is around the data of a sector that the image does not have */
static void cue_make_header(uint8_t *raw, uint32_t lba)
{
    raw[0] = 0x00;
    memset(&raw[1], 0xff, 10);
    raw[11] = 0x00;

    raw[12] = cue_bcd(lba / 4500u);
    raw[13] = cue_bcd((lba / 75u) % 60u);
    raw[14] = cue_bcd(lba % 75u);
    raw[15] = 0x02; /* mode 2 */
}

int32_t cue_query(cue_t *cue, uint32_t lba)
{
    if (lba >= cue_lead_out(cue))
        return TS_FAR;

    const cue_track_t *track = cue_track_at(cue, lba);

    // If the LBA isn't too far but the track wasn't found
    // then we are being requested a pregap sector.
    if (!track)
        return TS_PREGAP;

    return cue_kind(track, lba);
}

int32_t cue_read(cue_t *cue, uint32_t lba, void *buf)
{
    uint8_t *const raw = (uint8_t *)buf;

    if (lba >= cue_lead_out(cue))
        return TS_FAR;

    const cue_track_t *track = cue_track_at(cue, lba);

    /* ahead of the first track, or silence that is in no file */
    if (!track || (lba < track->first))
    {
        memset(raw, 0, CUE_RAW_BYTES);
        memset(raw + 1, 255, 10);

        return TS_PREGAP;
    }

    FIL *const fp = cue_file_handle(cue, track->file);

    if (!fp)
    {
        memset(raw, 0, CUE_RAW_BYTES);

        return cue_kind(track, lba);
    }

    const uint32_t offset = track->file_byte + ((lba - track->first) * track->sector_bytes);

    if (track->sector_bytes == CUE_RAW_BYTES)
    {
        cue_ra_read(fp, offset, raw, CUE_RAW_BYTES);
    }
    else if (track->sector_bytes == 2336u)
    {
        /* everything but the sync and the address */
        cue_make_header(raw, lba);
        cue_ra_read(fp, offset, &raw[16], 2336u);
    }
    else
    {
        /* the user data alone: a form 1 data sector around it (no error
           correction codes - nothing on this side of the drive checks them) */
        cue_make_header(raw, lba);

        raw[16] = raw[20] = 0x00; /* file    */
        raw[17] = raw[21] = 0x00; /* channel */
        raw[18] = raw[22] = 0x08; /* data    */
        raw[19] = raw[23] = 0x00; /* coding  */

        cue_ra_read(fp, offset, &raw[24], 2048u);

        memset(&raw[24 + 2048], 0, CUE_RAW_BYTES - (24u + 2048u));
    }

    return cue_kind(track, lba);
}

int32_t cue_get_track_number(cue_t *cue, uint32_t lba)
{
    if (!cue->track_count)
        return 1;

    /* the track it is in, pregap included; past the end, the last one */
    uint32_t found = 0;

    for (uint32_t i = 0; i < cue->track_count; i++)
    {
        if (lba >= cue->tracks[i].pre_start)
            found = i;
    }

    return cue->tracks[found].number;
}

int32_t cue_get_track_count(cue_t *cue)
{
    return (int32_t)cue->track_count;
}

int32_t cue_get_track_lba(cue_t *cue, int32_t track)
{
    /* track 0 asks for the lead out */
    if (!track)
        return (int32_t)cue_lead_out(cue);

    for (uint32_t i = 0; i < cue->track_count; i++)
    {
        if (cue->tracks[i].number == track)
            return (int32_t)cue->tracks[i].start;
    }

    return TS_FAR;
}

void cue_init_disc(cue_t *cue, psx_disc_t *disc)
{
    disc->udata = cue;
    disc->read_sector = (read_sector_func)cue_read;
    disc->query_sector = (query_sector_func)cue_query;
    disc->get_track_number = (get_track_number_func)cue_get_track_number;
    disc->get_track_count = (get_track_count_func)cue_get_track_count;
    disc->get_track_lba = (get_track_lba_func)cue_get_track_lba;
    disc->destroy = (destroy_func)cue_destroy;
}
