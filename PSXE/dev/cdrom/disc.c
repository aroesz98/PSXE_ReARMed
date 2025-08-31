#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "disc.h"
#include "cue.h"
#include "fsl_debug_console.h"

#define MSF_TO_LBA(m, s, f) ((m * 4500) + (s * 75) + f)

const char *disc_cd_extensions[] = {
    "cue",
    "bin",
    "iso",
    0};

psx_disc_t *psx_disc_create(void)
{
    return malloc(sizeof(psx_disc_t));
}

int32_t disc_get_extension(const char *path)
{
    const char *ptr = &path[strlen(path) - 1];
    int32_t i = 0;

    while ((*ptr != '.') && (ptr != path))
        --ptr;

    if (ptr == path)
        return CD_EXT_UNSUPPORTED;

    while (disc_cd_extensions[i])
    {
        if (!strcmp(ptr + 1, disc_cd_extensions[i]))
            return i;

        ++i;
    }

    return CD_EXT_UNSUPPORTED;
}

int32_t disc_get_cd_type(psx_disc_t *disc)
{
    char buf[CD_SECTOR_SIZE];

    // If the disc is smaller than 16 sectors
    // then it can't be a PlayStation game.
    // Audio discs should also have ISO volume
    // descriptors, so it's probably something else
    // entirely.
    if (!psx_disc_read(disc, MSF_TO_LBA(0, 2, 16), buf))
        return CDT_UNKNOWN;

    // Check for the "PLAYSTATION" string at PVD offset 20h
    // Patch 20 byte so comparison is done correctly
    buf[0x2b] = 0;

    if (strncmp(&buf[0x20], "PLAYSTATION", 12))
        return CDT_AUDIO;

    return CDT_LICENSED;
}

int32_t psx_disc_open(psx_disc_t *disc, const char *path)
{
    PRINTF("[DISC] Opening disc: %s\r\n", path ? path : "NULL");

    if (!path)
    {
        PRINTF("[DISC] Error: Path is NULL\r\n");
        return CDT_ERROR;
    }

    PRINTF("[DISC] Getting file extension...\r\n");
    int32_t ext = disc_get_extension(path);
    PRINTF("[DISC] File extension type: %d\r\n", ext);

    return psx_disc_open_as(disc, path, ext);
}

int32_t psx_disc_open_as(psx_disc_t *disc, const char *path, int32_t type)
{
    PRINTF("[DISC] Opening disc as type: %d\r\n", type);

    switch (type)
    {
    case CD_EXT_CUE:
    {
        PRINTF("[DISC] Processing CUE file...\r\n");
        cue_t *cue = cue_create();

        if (!cue)
        {
            PRINTF("[DISC] Error: Failed to create CUE instance\r\n");
            return CDT_ERROR;
        }

        PRINTF("[DISC] Initializing CUE...\r\n");
        cue_init(cue);
        cue_init_disc(cue, disc);

        PRINTF("[DISC] Parsing CUE file: %s\r\n", path);
        if (cue_parse(cue, path))
        {
            PRINTF("[DISC] Error: CUE parsing failed\r\n");
            return CDT_ERROR;
        }

        PRINTF("[DISC] Loading CUE data...\r\n");
        if (cue_load(cue, LD_FILE))
        {
            PRINTF("[DISC] Error: CUE loading failed\r\n");
            return CDT_ERROR;
        }

        PRINTF("[DISC] CUE file processed successfully\r\n");
    }
    break;
    default:
        PRINTF("[DISC] Unsupported disc type: %d\r\n", type);
        return CDT_ERROR;
    }

    int32_t cd_type = disc_get_cd_type(disc);
    PRINTF("[DISC] Detected CD type: %d\r\n", cd_type);
    return cd_type;
}

int32_t psx_disc_read(psx_disc_t *disc, uint32_t lba, void *buf)
{
    return disc->read_sector(disc->udata, lba, buf);
}

int32_t psx_disc_query(psx_disc_t *disc, uint32_t lba)
{
    return disc->query_sector(disc->udata, lba);
}

int32_t psx_disc_get_track_number(psx_disc_t *disc, uint32_t lba)
{
    return disc->get_track_number(disc->udata, lba);
}

int32_t psx_disc_get_track_count(psx_disc_t *disc)
{
    return disc->get_track_count(disc->udata);
}

int32_t psx_disc_get_track_lba(psx_disc_t *disc, int32_t track)
{
    return disc->get_track_lba(disc->udata, track);
}

void psx_disc_destroy(psx_disc_t *disc)
{
    disc->destroy(disc->udata);

    free(disc);
}
