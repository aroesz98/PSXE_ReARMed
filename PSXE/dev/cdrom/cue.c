#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "cue.h"

/* FATFS includes */
#include "ff.h"
#include "fsl_debug_console.h"

#define CUE_BUFFER_SIZE 4096

static cue_track_t sCue_track;
static cue_file_t sCue_file;
static cue_t sCue;

/* FATFS helper functions for character-by-character reading */
static int32_t cue_load_buffer(cue_t *cue)
{
    UINT bytesRead;
    FRESULT res = f_read(&cue->file, cue->file_buffer, CUE_BUFFER_SIZE, &bytesRead);
    if (res != FR_OK || bytesRead == 0)
    {
        return 0; /* EOF or error */
    }
    cue->buffer_size = bytesRead;
    cue->buffer_pos = 0;
    return 1;
}

static int32_t cue_fgetc(cue_t *cue)
{
    if (cue->buffer_pos >= cue->buffer_size)
    {
        if (!cue_load_buffer(cue))
        {
            return EOF;
        }
    }
    return (unsigned char)cue->file_buffer[cue->buffer_pos++];
}

static int32_t cue_feof(cue_t *cue)
{
    /* Check if we're at end of buffer and can't load more */
    if (cue->buffer_pos >= cue->buffer_size)
    {
        /* Try to load more data */
        return !cue_load_buffer(cue);
    }
    return 0; /* Still have data in buffer */
}

static const char *cue_keywords[] = {
    "4CH",
    "AIFF",
    "AUDIO",
    "BINARY",
    "CATALOG",
    "CDG",
    "CDI/2336",
    "CDI/2352",
    "CDTEXTFILE",
    "DCP",
    "FILE",
    "FLAGS",
    "INDEX",
    "ISRC",
    "MODE1/2048",
    "MODE1/2352",
    "MODE2/2336",
    "MODE2/2352",
    "MOTOROLA",
    "MP3",
    "PERFORMER",
    "POSTGAP",
    "PRE",
    "PREGAP",
    "REM",
    "SCMS",
    "SONGWRITER",
    "TITLE",
    "TRACK",
    "WAVE",
    0};

char *strapp(char *dst, const char *a, const char *b)
{
    char *d = dst;

    while (*a)
        *dst++ = *a++;

    while (*b)
        *dst++ = *b++;

    *dst = '\0';

    return d;
}

const char *find_last_slash(const char *a)
{
    if (!a)
        return NULL;

    const char *b = a;

    while (*a)
    {
        if (*a == '/' || *a == '\\')
            b = a + 1;

        ++a;
    }

    return b;
}

char *get_root_path(char *dst, const char *a)
{
    if (!a)
    {
        *dst = '\0';

        return dst;
    }

    const char *b = a;
    const char *c = a;
    char *d = dst;

    while (*a)
    {
        if (*a == '/' || *a == '\\')
            b = a + 1;

        ++a;
    }

    while (c != b)
        *dst++ = *c++;

    *dst = '\0';

    return d;
}

int32_t cue_parse_keyword(cue_t *cue)
{
    char buf[256];
    char *ptr = buf;

    while (isalpha((unsigned char)cue->c) || isdigit((unsigned char)cue->c) || cue->c == '/')
    {
        *ptr++ = cue->c;

        cue->c = cue_fgetc(cue);
    }

    *ptr = '\0';

    int32_t i = 0;

    const char *keyword = cue_keywords[i];

    while (keyword)
    {
        if (!strcmp(keyword, buf))
        {
            return i;
        }
        else
        {
            keyword = cue_keywords[++i];
        }
    }

    return -1;
}

int32_t cue_parse_number(cue_t *cue)
{
    if (!isdigit((unsigned char)cue->c))
        return 0;

    char buf[4];

    char *ptr = buf;

    while (isdigit((unsigned char)cue->c))
    {
        *ptr++ = cue->c;

        cue->c = cue_fgetc(cue);
    }

    *ptr = '\0';

    return atoi(buf);
}

uint32_t cue_parse_msf(cue_t *cue)
{
    int32_t m = 0;
    int32_t s = 0;
    int32_t f = 0;

    if (!isdigit((unsigned char)cue->c))
        return 0;

    m = cue_parse_number(cue);

    if (cue->c != ':')
        return 0;

    cue->c = cue_fgetc(cue);

    s = cue_parse_number(cue);

    if (cue->c != ':')
        return 0;

    cue->c = cue_fgetc(cue);

    f = cue_parse_number(cue);

    // 1 second = 75 frames (sectors)
    // 1 minute = 60 seconds = 4500 frames
    return f + (s * 75) + (m * 4500);
}

void cue_parse_index(cue_t *cue)
{
    cue_track_t *track = list_back(cue->tracks)->data;

    while (isspace((unsigned char)cue->c))
        cue->c = cue_fgetc(cue);

    if (!isdigit((unsigned char)cue->c))
        return;

    int32_t i = cue_parse_number(cue);

    while (isspace((unsigned char)cue->c))
        cue->c = cue_fgetc(cue);

    if (i > 1)
        return;

    track->index[i] = cue_parse_msf(cue);
}

cue_track_t *cue_parse_track(cue_t *cue)
{
    while (isspace((unsigned char)cue->c))
        cue->c = cue_fgetc(cue);

    if (!isdigit((unsigned char)cue->c))
        return NULL;

    cue_track_t *track = &sCue_track;

    track->end = 0;
    track->start = 0;
    track->pregap = 0;
    track->index[0] = -1;
    track->index[1] = -1;
    track->file = list_back(cue->files)->data;
    track->number = cue_parse_number(cue);

    while (isspace((unsigned char)cue->c))
        cue->c = cue_fgetc(cue);

    track->mode = cue_parse_keyword(cue);

    return track;
}

cue_file_t *cue_parse_file(cue_t *cue, const char *p, const char *s)
{
    while (isspace((unsigned char)cue->c))
        cue->c = cue_fgetc(cue);

    if (cue->c != '\"')
        return NULL;

    cue_file_t *file = &sCue_file;

    file->tracks = list_create();
    file->name = malloc(512);

    // Append root path to track file path
    char *ptr = file->name;

    while (p != s)
        *ptr++ = *p++;

    cue->c = cue_fgetc(cue);

    while (cue->c != '\"')
    {
        *ptr++ = cue->c;

        cue->c = cue_fgetc(cue);
    }

    *ptr = '\0';

    cue->c = cue_fgetc(cue);

    // Ignore file type
    while (isspace((unsigned char)cue->c))
        cue->c = cue_fgetc(cue);

    while (isalpha((unsigned char)cue->c))
        cue->c = cue_fgetc(cue);

    return file;
}

cue_t *cue_create(void)
{
    return &sCue;
}

void cue_init(cue_t *cue)
{
    cue->files = list_create();
    cue->tracks = list_create();
    cue->file_buffer = NULL;
    cue->buffer_pos = 0;
    cue->buffer_size = 0;
}

int32_t cue_parse(cue_t *cue, const char *path)
{
    FRESULT res;

    res = f_open(&cue->file, path, FA_READ);
    if (res != FR_OK)
        return CUE_FILE_NOT_FOUND;

    /* Allocate buffer for file reading */
    cue->file_buffer = malloc(CUE_BUFFER_SIZE);
    if (!cue->file_buffer)
    {
        f_close(&cue->file);
        return CUE_FILE_NOT_FOUND;
    }
    cue->buffer_pos = 0;
    cue->buffer_size = 0;

    const char *s = find_last_slash(path);

    cue->c = cue_fgetc(cue);

    while (isspace((unsigned char)cue->c))
        cue->c = cue_fgetc(cue);

    while (!cue_feof(cue) && cue->c != EOF)
    {
        int32_t kw = cue_parse_keyword(cue);

        switch (kw)
        {
        case CUE_FILE:
        {
            list_push_back(cue->files, cue_parse_file(cue, path, s));
        }
        break;

        case CUE_TRACK:
        {
            cue_track_t *track = cue_parse_track(cue);
            cue_file_t *file = list_back(cue->files)->data;

            list_push_back(cue->tracks, track);
            list_push_back(file->tracks, track);
        }
        break;

        case CUE_INDEX:
        {
            cue_parse_index(cue);
        }
        break;

        case CUE_REM:
        case CUE_PREGAP:
        case CUE_FLAGS:
        case CUE_POSTGAP:
        {
            // Ignore everything until a newline (handle CRLF and LF)
            while ((cue->c != '\n') && (cue->c != '\r'))
                cue->c = cue_fgetc(cue);

            while ((cue->c == '\n') && (cue->c == '\r'))
                cue->c = cue_fgetc(cue);
        }
        break;

        default:
        {
            PRINTF("Unknown keyword: %s (%u)\r\n", cue_keywords[kw], kw);

            return 1;
        }
        break;
        }

        while (isspace((unsigned char)cue->c))
            cue->c = cue_fgetc(cue);
    }

    /* Close the file and cleanup buffer */
    f_close(&cue->file);
    if (cue->file_buffer)
    {
        free(cue->file_buffer);
        cue->file_buffer = NULL;
    }

    return 0;
}

uint32_t get_file_size(FIL *file)
{
    return f_size(file);
}

/*
(0   - 0  )                   = 150
(0   - 0  ) + 150    + 315000 = 315150
(0   - 0  ) + 315150 + 375    = 315525
(150 - 0  ) + 315525 + 390    = 316065
(195 - 150) + 316065 + 390    = 316500
(155 - 190) + 316500 + 390    =
*/

int32_t prev_pregap = 0;

uint32_t init_tracks(cue_file_t *file, uint32_t *lba)
{
    node_t *node = list_front(file->tracks);

    // 1 track per file case
    if (file->tracks->size == 1)
    {
        cue_track_t *data = node->data;

        data->pregap = 0;

        if ((data->index[0] != -1) && (data->index[1] != -1))
            data->pregap = data->index[1];

        data->start = *lba + data->pregap;
        data->end = data->start + (file->size / 0x930);

        *lba = data->end;

        return 0;
    }

    // Multiple tracks per file
    while (node)
    {
        cue_track_t *data = node->data;

        // If this is the last track
        if (!node->next)
        {
            data->pregap = 0;
            data->start = data->index[1] + 150;
            data->end = file->size / 0x930;

            return 0;
        }

        cue_track_t *next = node->next->data;

        data->start = data->index[1] + 150;
        data->end = (next->index[1] + 150) - 1;
        data->pregap = 0;

        node = node->next;
    }

    return 0;
}

int32_t cue_load(cue_t *cue, int32_t mode)
{
    node_t *node = list_front(cue->files);

    // 00:02:00
    uint32_t lba = 2 * 75;

    while (node)
    {
        cue_file_t *data = node->data;

        FIL file;
        FRESULT res = f_open(&file, data->name, FA_READ);

        if (res != FR_OK)
            return CUE_TRACK_FILE_NOT_FOUND;

        data->buf_mode = mode;
        data->size = get_file_size(&file);

        PRINTF("Loaded \'%s\': size=0x%X, sectors=0x%X\r\n",
               data->name,
               data->size,
               data->size / 0x930);

        if (data->buf_mode == LD_BUFFERED)
        {
            data->buf = malloc(data->size);

            UINT bytesRead;
            res = f_read(&file, data->buf, data->size, &bytesRead);
            if (res != FR_OK || bytesRead != data->size)
            {
                f_close(&file);
                return CUE_TRACK_READ_ERROR;
            }

            f_close(&file);
        }
        else
        {
            // For LD_FILE mode, we need to keep a file handle
            data->buf = malloc(sizeof(FIL));
            memcpy(data->buf, &file, sizeof(FIL));
        }

        data->start = lba;

        init_tracks(data, &lba);

        node = node->next;
    }

    return CUE_OK;
}

void cue_destroy(cue_t *cue)
{
    node_t *node = list_front(cue->files);

    while (node)
    {
        cue_file_t *file = node->data;

        if (file->buf_mode == LD_BUFFERED)
        {
            free(file->buf);
        }
        else
        {
            f_close((FIL *)file->buf);
            free(file->buf);
        }

        list_destroy(file->tracks);

        free(file->name);
        memset(&sCue_file, 0, sizeof(cue_file_t));

        node = node->next;
    }

    list_destroy(cue->files);

    node = list_front(cue->tracks);

    while (node)
    {
        free(node->data);

        node = node->next;
    }

    list_destroy(cue->tracks);

    /* Clean up file buffer and close file if still open */
    if (cue->file_buffer)
    {
        free(cue->file_buffer);
    }

    free(cue);
}

cue_track_t *get_sector_track(cue_t *cue, uint32_t lba)
{
    node_t *node = list_front(cue->tracks);

    while (node)
    {
        cue_track_t *track = node->data;

        if ((lba >= track->start) && (lba < track->end))
            return track;

        node = node->next;
    }

    return NULL;
}

cue_track_t *get_sector_track_in_pregap(cue_t *cue, uint32_t lba)
{
    node_t *node = list_front(cue->tracks);

    while (node)
    {
        cue_track_t *track = node->data;

        if (!node->next)
            return track;

        cue_track_t *next = node->next->data;

        // Ignore sector number
        int32_t curr_start = track->start - (track->start % 75);
        int32_t next_start = next->start - (next->start % 75);

        if ((lba >= curr_start) && (lba < next_start))
            return track;

        node = node->next;
    }

    return NULL;
}

int32_t cue_query(cue_t *cue, uint32_t lba)
{
    if (lba >= ((cue_track_t *)list_back(cue->tracks)->data)->end)
        return TS_FAR;

    cue_track_t *track = get_sector_track(cue, lba);

    // If the LBA isn't too far but the track wasn't found
    // then we are being requested a pregap sector. Clear buffer
    // and initialize sync data (not actually needed)
    if (!track)
        return TS_PREGAP;

    return (track->mode == CUE_MODE2_2352) ? TS_DATA : TS_AUDIO;
}

int32_t cue_read(cue_t *cue, uint32_t lba, void *buf)
{
    if (lba >= ((cue_track_t *)list_back(cue->tracks)->data)->end)
        return TS_FAR;

    cue_track_t *track = get_sector_track(cue, lba);

    // If the LBA isn't too far but the track wasn't found
    // then we are being requested a pregap sector. Clear buffer
    // and initialize sync data (not actually needed)
    if (!track)
    {
        memset((uint8_t *)buf, 0, 2352);
        memset((uint8_t *)buf + 1, 255, 10);

        return TS_PREGAP;
    }

    cue_file_t *file = track->file;

    // PRINTF("Reading sector %u at track %u, file=%s (%u), offset=%u (%08x)\r\n",
    //     lba,
    //     track->number,
    //     track->file->name,
    //     file->start,
    //     lba - file->start,
    //     (lba - file->start) * 2352
    // );

    if (file->buf_mode == LD_BUFFERED)
    {
        uint8_t *ptr = (uint8_t *)file->buf + ((lba - file->start) * 2352);

        memcpy(buf, ptr, 2352);
    }
    else
    {
        DWORD offset = (lba - file->start) * 2352;
        f_lseek((FIL *)file->buf, offset);

        UINT bytesRead;
        f_read((FIL *)file->buf, buf, 2352, &bytesRead);
    }

    return (track->mode == CUE_MODE2_2352) ? TS_DATA : TS_AUDIO;
}

int32_t cue_get_track_number(cue_t *cue, uint32_t lba)
{
    cue_track_t *track = get_sector_track_in_pregap(cue, lba);

    return track->number;
}

int32_t cue_get_track_count(cue_t *cue)
{
    return cue->tracks->size;
}

int32_t cue_get_track_lba(cue_t *cue, int32_t track)
{
    if (!track)
        return ((cue_track_t *)list_back(cue->tracks)->data)->end;

    if (track > cue->tracks->size)
        return TS_FAR;

    cue_track_t *data = list_at(cue->tracks, track - 1)->data;

    return data->start;
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
