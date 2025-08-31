#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "exp1.h"

/* FATFS includes */
#include "ff.h"

// Static buffer for EXP1 instance
static psx_exp1_t g_exp1_instance;
static uint8_t __attribute__((section(".ramfunc.$BOARD_SDRAM"))) g_exp1_rom_buffer[PSX_EXP1_SIZE];
static int32_t g_exp1_instance_used = 0;

psx_exp1_t *psx_exp1_create(void)
{
    if (g_exp1_instance_used)
    {
        return NULL; // Only one instance allowed
    }
    g_exp1_instance_used = 1;
    return &g_exp1_instance;
}

int32_t psx_exp1_init(psx_exp1_t *exp1, psx_mc1_t *mc1, const char *path)
{
    memset(exp1, 0, sizeof(psx_exp1_t));

    exp1->io_base = PSX_EXP1_BEGIN;
    exp1->io_size = PSX_EXP1_SIZE;

    exp1->mc1 = mc1;
    exp1->rom = g_exp1_rom_buffer;

    memset(exp1->rom, 0xff, PSX_EXP1_SIZE);

    if (path)
        return psx_exp1_load(exp1, path);

    return 0;
}

int32_t psx_exp1_load(psx_exp1_t *exp1, const char *path)
{
    if (!path)
        return 0;

    FIL file;
    FRESULT res;

    res = f_open(&file, path, FA_READ);
    if (res != FR_OK)
        return 1;

    UINT bytesRead;
    res = f_read(&file, exp1->rom, PSX_EXP1_SIZE, &bytesRead);
    if (res != FR_OK)
    {
        f_close(&file);
        return 2;
    }

    f_close(&file);

    return 0;
}

uint32_t psx_exp1_read32(psx_exp1_t *exp1, uint32_t offset)
{
    return *((uint32_t *)(exp1->rom + offset));
}

uint16_t psx_exp1_read16(psx_exp1_t *exp1, uint32_t offset)
{
    return *((uint16_t *)(exp1->rom + offset));
}

uint8_t psx_exp1_read8(psx_exp1_t *exp1, uint32_t offset)
{
    return exp1->rom[offset];
}

void psx_exp1_write32(psx_exp1_t *exp1, uint32_t offset, uint32_t value)
{
    log_warn("Unhandled 32-bit EXP1 write at offset %08x (%08x)", offset, value);
}

void psx_exp1_write16(psx_exp1_t *exp1, uint32_t offset, uint16_t value)
{
    log_warn("Unhandled 16-bit EXP1 write at offset %08x (%04x)", offset, value);
}

void psx_exp1_write8(psx_exp1_t *exp1, uint32_t offset, uint8_t value)
{
    log_warn("Unhandled 8-bit EXP1 write at offset %08x (%02x)", offset, value);
}

void psx_exp1_destroy(psx_exp1_t *exp1)
{
    // Mark instance as available again
    g_exp1_instance_used = 0;
}