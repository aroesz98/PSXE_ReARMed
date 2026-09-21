#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "disc.h"
#include "cue.h"
#include "fsl_debug_console.h"

#define MSF_TO_LBA(m, s, f) ((m * 4500) + (s * 75) + f)

/* where the 2048 bytes of user data are in a raw mode 2 form 1 sector */
#define DISC_DATA 24

#ifndef DISC_SDRAM
#define DISC_SDRAM __attribute__((section(".bss.$BOARD_SDRAM")))
#endif

static const char *const disc_cd_extensions[] = {
    "cue",
    "bin",
    "iso",
    "img",
    0};

/* why the last psx_disc_open() said CDT_ERROR, in words a player can use */
static const char *g_disc_error = "";

const char *psx_disc_last_error(void)
{
    return g_disc_error;
}

psx_disc_t *psx_disc_create(void)
{
    return malloc(sizeof(psx_disc_t));
}

int32_t disc_get_extension(const char *path)
{
    const char *dot = NULL;

    for (const char *p = path; *p; p++)
    {
        if (*p == '.')
            dot = p;
        else if ((*p == '/') || (*p == '\\'))
            dot = NULL;
    }

    if (!dot)
        return CD_EXT_UNSUPPORTED;

    for (int32_t i = 0; disc_cd_extensions[i]; i++)
    {
        const char *a = dot + 1;
        const char *b = disc_cd_extensions[i];

        while (*a && *b && (tolower((unsigned char)*a) == *b))
        {
            a++;
            b++;
        }

        if (!*a && !*b)
            return (i == 3) ? CD_EXT_BIN : i; /* an .img is a .bin by another name */
    }

    return CD_EXT_UNSUPPORTED;
}

static inline uint32_t disc_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
    The first sector of a file in the root directory of the disc's ISO 9660
    volume, into `sector` (a raw one, the data is at DISC_DATA). Returns the size
    of the file, 0 when there is no such file. `name` comes with its ";1".
*/
static uint32_t disc_read_root_file(psx_disc_t *disc, const char *name, uint8_t *sector)
{
    if (!psx_disc_read(disc, MSF_TO_LBA(0, 2, 16), sector))
        return 0;

    const uint8_t *const pvd = &sector[DISC_DATA];

    if ((pvd[0] != 1u) || memcmp(&pvd[1], "CD001", 5))
        return 0;

    /* the root directory record is at 156 of the volume descriptor */
    uint32_t dir_lba = disc_le32(&pvd[156 + 2]);
    uint32_t dir_left = disc_le32(&pvd[156 + 10]);

    const uint32_t name_len = (uint32_t)strlen(name);

    /* a root directory is a sector or two; a damaged one is not followed far */
    for (uint32_t n = 0; (n < 16u) && dir_left; n++, dir_lba++)
    {
        if (!psx_disc_read(disc, dir_lba + 150u, sector))
            return 0;

        const uint8_t *const dir = &sector[DISC_DATA];

        for (uint32_t at = 0; at < 2048u;)
        {
            const uint32_t len = dir[at];

            if (!len || ((at + len) > 2048u))
                break; /* the rest of the sector is padding */

            const uint32_t id_len = dir[at + 32];

            if ((id_len == name_len) && ((at + 33u + id_len) <= 2048u) && !memcmp(&dir[at + 33], name, name_len))
            {
                const uint32_t file_lba = disc_le32(&dir[at + 2]);
                const uint32_t file_size = disc_le32(&dir[at + 10]);

                if (!psx_disc_read(disc, file_lba + 150u, sector))
                    return 0;

                return file_size;
            }

            at += len;
        }

        dir_left = (dir_left > 2048u) ? (dir_left - 2048u) : 0u;
    }

    return 0;
}

/* A PlayStation 2 disc carries the same "PLAYSTATION" system identifier, and is
   told apart by what it boots: SYSTEM.CNF says BOOT2 instead of BOOT. */
static int disc_is_ps2(psx_disc_t *disc, uint8_t *sector)
{
    uint32_t size = disc_read_root_file(disc, "SYSTEM.CNF;1", sector);

    if (!size)
        return 0;

    if (size > 2048u)
        size = 2048u;

    const char *const text = (const char *)&sector[DISC_DATA];

    for (uint32_t i = 0; (i + 5u) <= size; i++)
    {
        if (!memcmp(&text[i], "BOOT2", 5))
            return 1;
    }

    return 0;
}

int32_t disc_get_cd_type(psx_disc_t *disc)
{
    /* too much for the stack of the task that opens discs, and nothing that
       has to be in fast memory */
    static uint8_t DISC_SDRAM buf[CD_SECTOR_SIZE];

    // If the disc is smaller than 16 sectors
    // then it can't be a PlayStation game.
    // Audio discs should also have ISO volume
    // descriptors, so it's probably something else
    // entirely.
    if (!psx_disc_read(disc, MSF_TO_LBA(0, 2, 16), buf))
        return CDT_UNKNOWN;

    // Check for the "PLAYSTATION" string at PVD offset 20h
    if (strncmp((const char *)&buf[0x20], "PLAYSTATION", 11))
        return CDT_AUDIO;

    if (disc_is_ps2(disc, buf))
    {
        g_disc_error = "This is a PlayStation 2 disc. Only PlayStation 1 games can run here.";

        return CDT_ERROR;
    }

    return CDT_LICENSED;
}

int32_t psx_disc_open(psx_disc_t *disc, const char *path)
{
    PRINTF("[DISC] Opening disc: %s\r\n", path ? path : "NULL");

    if (!path)
    {
        PRINTF("[DISC] Error: Path is NULL\r\n");
        g_disc_error = "No disc image was chosen.";
        return CDT_ERROR;
    }

    return psx_disc_open_as(disc, path, disc_get_extension(path));
}

int32_t psx_disc_open_as(psx_disc_t *disc, const char *path, int32_t type)
{
    g_disc_error = "";

    disc->udata = NULL;
    disc->destroy = NULL;

    if ((type != CD_EXT_CUE) && (type != CD_EXT_BIN) && (type != CD_EXT_ISO))
    {
        PRINTF("[DISC] Unsupported disc type: %d\r\n", (int)type);
        g_disc_error = "This kind of file is not a disc image.";
        return CDT_ERROR;
    }

    cue_t *cue = cue_create();

    if (!cue)
    {
        g_disc_error = "Out of memory.";
        return CDT_ERROR;
    }

    cue_init(cue);
    cue_init_disc(cue, disc);

    /* a cue sheet says what is where; an image on its own is one data track */
    int32_t result = (type == CD_EXT_CUE) ? cue_parse(cue, path) : cue_open_image(cue, path);

    if (result == CUE_OK)
        result = cue_load(cue, LD_FILE);

    if (result != CUE_OK)
    {
        PRINTF("[DISC] Error %d opening %s\r\n", (int)result, path);

        switch (result)
        {
        case CUE_TRACK_FILE_NOT_FOUND:
            g_disc_error = "A track file this cue sheet names is not on the card.";
            break;
        case CUE_BAD_SHEET:
            g_disc_error = "This cue sheet has no tracks in it.";
            break;
        case CUE_NINTENDO:
            g_disc_error = "This is a Nintendo GameCube or Wii disc. Only PlayStation 1 games can run here.";
            break;
        case CUE_BAD_IMAGE:
            g_disc_error = "This file is not a CD image: neither raw sectors nor ISO 9660.";
            break;
        default:
            g_disc_error = "The image could not be read from the card.";
            break;
        }

        return CDT_ERROR;
    }

    int32_t cd_type = disc_get_cd_type(disc);
    PRINTF("[DISC] Detected CD type: %d, %d track(s)\r\n", (int)cd_type, (int)psx_disc_get_track_count(disc));
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
    if (disc->destroy && disc->udata)
        disc->destroy(disc->udata);

    free(disc);
}

int32_t psx_disc_probe(const char *path)
{
    psx_disc_t *disc = psx_disc_create();

    if (!disc)
    {
        g_disc_error = "Out of memory.";

        return CDT_ERROR;
    }

    const int32_t type = psx_disc_open(disc, path);

    psx_disc_destroy(disc);

    return type;
}
