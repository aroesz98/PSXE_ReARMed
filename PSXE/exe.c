#include <stdio.h>

#include "exe.h"
#include "log.h"

/* FATFS includes */
#include "ff.h"

int32_t psx_exe_load(psx_cpu_t *cpu, const char *path)
{
    if (!path)
        return 0;

    FIL file;
    FRESULT res;

    res = f_open(&file, path, FA_READ);
    if (res != FR_OK)
        return 1;

    // Read header
    psx_exe_hdr_t hdr;
    UINT bytesRead;

    res = f_read(&file, (char *)&hdr, sizeof(psx_exe_hdr_t), &bytesRead);
    if (res != FR_OK || bytesRead != sizeof(psx_exe_hdr_t))
    {
        f_close(&file);
        return 2;
    }

    // Seek to program start
    res = f_lseek(&file, 0x800);
    if (res != FR_OK)
    {
        f_close(&file);
        return 3;
    }

    // Read to RAM directly
    uint32_t offset = hdr.ramdest & 0x7fffffff;

    res = f_read(&file, cpu->bus->ram->buf + offset, hdr.filesz, &bytesRead);
    if (res != FR_OK || bytesRead != hdr.filesz)
    {
        f_close(&file);
        return 3;
    }

    // Load initial register values
    cpu->pc = hdr.ipc;
    cpu->next_pc = cpu->pc + 4;
    cpu->r[28] = hdr.igp;

    if (hdr.ispb)
    {
        cpu->r[29] = hdr.ispb + hdr.ispoff;
        cpu->r[30] = cpu->r[29];
    }

    log_info("Loaded PS-X EXE file \"%s\"", path);
    log_fatal("PC=%08x SP=%08x (%08x) GP=%08x", cpu->pc, cpu->r[29], hdr.ispb, cpu->r[28]);

    f_close(&file);

    return 0;
}