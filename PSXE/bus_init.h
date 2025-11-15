#ifndef BUS_INIT_H
#define BUS_INIT_H

#include <stdint.h>

#include "dev/cdrom/cdrom.h"
#include "dev/bios.h"
#include "dev/ram.h"
#include "dev/dma.h"
#include "dev/exp1.h"
#include "dev/exp2.h"
#include "dev/mc1.h"
#include "dev/mc2.h"
#include "dev/mc3.h"
#include "dev/ic.h"
#include "dev/scratchpad.h"
#include "dev/gpu.h"
#include "dev/spu.h"
#include "dev/timer.h"
#include "dev/pad.h"
#include "dev/mdec.h"

typedef enum
{
    PSX_BUS_DEVICE_NONE = 0,
    PSX_BUS_DEVICE_BIOS,
    PSX_BUS_DEVICE_RAM,
    PSX_BUS_DEVICE_DMA,
    PSX_BUS_DEVICE_EXP1,
    PSX_BUS_DEVICE_EXP2,
    PSX_BUS_DEVICE_MC1,
    PSX_BUS_DEVICE_MC2,
    PSX_BUS_DEVICE_MC3,
    PSX_BUS_DEVICE_IC,
    PSX_BUS_DEVICE_SCRATCHPAD,
    PSX_BUS_DEVICE_GPU,
    PSX_BUS_DEVICE_SPU,
    PSX_BUS_DEVICE_TIMER,
    PSX_BUS_DEVICE_CDROM,
    PSX_BUS_DEVICE_PAD,
    PSX_BUS_DEVICE_MDEC,
} psx_bus_device_kind_t;

typedef struct
{
    psx_bus_device_kind_t kind;
    uint32_t base;
    uint32_t size;
    uint32_t delay;
} psx_bus_cache_t;

struct psx_bus_t
{
    psx_bios_t *bios;
    psx_ram_t *ram;
    psx_dma_t *dma;
    psx_exp1_t *exp1;
    psx_exp2_t *exp2;
    psx_mc1_t *mc1;
    psx_mc2_t *mc2;
    psx_mc3_t *mc3;
    psx_ic_t *ic;
    psx_scratchpad_t *scratchpad;
    psx_gpu_t *gpu;
    psx_spu_t *spu;
    psx_timer_t *timer;
    psx_cdrom_t *cdrom;
    psx_pad_t *pad;
    psx_mdec_t *mdec;

    uint32_t access_cycles;
    psx_bus_cache_t cache;
};

void psx_bus_init_bios(psx_bus_t *, psx_bios_t *);
void psx_bus_init_ram(psx_bus_t *, psx_ram_t *);
void psx_bus_init_dma(psx_bus_t *, psx_dma_t *);
void psx_bus_init_exp1(psx_bus_t *, psx_exp1_t *);
void psx_bus_init_exp2(psx_bus_t *, psx_exp2_t *);
void psx_bus_init_mc1(psx_bus_t *, psx_mc1_t *);
void psx_bus_init_mc2(psx_bus_t *, psx_mc2_t *);
void psx_bus_init_mc3(psx_bus_t *, psx_mc3_t *);
void psx_bus_init_ic(psx_bus_t *, psx_ic_t *);
void psx_bus_init_scratchpad(psx_bus_t *, psx_scratchpad_t *);
void psx_bus_init_gpu(psx_bus_t *, psx_gpu_t *);
void psx_bus_init_spu(psx_bus_t *, psx_spu_t *);
void psx_bus_init_timer(psx_bus_t *, psx_timer_t *);
void psx_bus_init_cdrom(psx_bus_t *, psx_cdrom_t *);
void psx_bus_init_pad(psx_bus_t *, psx_pad_t *);
void psx_bus_init_mdec(psx_bus_t *, psx_mdec_t *);

#endif