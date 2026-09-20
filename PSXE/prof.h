#ifndef PSX_PROF_H
#define PSX_PROF_H

/* Lightweight DWT-based profiler for the PSX emulator hot loop.
   Set PSX_PROFILE to 0 to compile out completely. */

#ifndef PSX_PROFILE
#define PSX_PROFILE 0
#endif

#include <stdint.h>

#if PSX_PROFILE

#include "fsl_device_registers.h"

typedef struct
{
    uint32_t instr;    /* emulated instructions */
    uint32_t ecycles;  /* emulated CPU cycles */
    uint32_t cpu;      /* core cycles in psx_cpu_cycle (incl. GP0/DMA work) */
    uint32_t dev;      /* core cycles in device updates (incl. blit) */
    uint32_t d_cdrom;  /* per device breakdown of the update round */
    uint32_t d_gpu;
    uint32_t d_pad;
    uint32_t d_timer;
    uint32_t d_dma;
    uint32_t gp0;      /* core cycles in GP0 command execution (rasterizer) */
    uint32_t blit;     /* core cycles in screen update (scale + LCD) */
    uint32_t dmax;     /* core cycles in DMA block transfers */
    uint32_t bwait;    /* core cycles spent waiting for PXP */
    uint32_t frames;   /* screen updates */
    uint32_t gp0cmds;  /* GP0 commands executed */
    uint32_t pixels;   /* rasterized pixels (bounding box estimate) */
    uint32_t px_flat;  /* flat untextured */
    uint32_t px_shade; /* gouraud, untextured */
    uint32_t px_t4;    /* textured, 4bpp palette */
    uint32_t px_t8;    /* textured, 8bpp palette */
    uint32_t px_t15;   /* textured, 15bpp direct */
    uint32_t px_rect;  /* rectangles (any kind) */
    uint32_t px_fast;  /* pixels through the specialised paletted span loop */
    uint32_t px_transp;/* pixels of primitives with semi transparency enabled */
    uint32_t px_raw;   /* pixels of primitives with raw texture mode */
    uint32_t t_start;  /* CYCCNT at window start */
} psx_prof_t;

extern psx_prof_t g_prof;

/* Small I/O access trace, armed automatically when the emulated machine stops
   producing frames, so we can see what it is polling while it is stuck. */
#define PSX_IO_TRACE_SIZE 192

typedef struct
{
    uint32_t addr;
    uint32_t value;
    uint8_t write;
    uint8_t size;
} psx_io_trace_t;

extern psx_io_trace_t g_io_trace[PSX_IO_TRACE_SIZE];
extern volatile uint32_t g_io_trace_idx;
extern volatile int32_t g_io_trace_on;

#define PSX_IO_TRACE(a, v, w, sz)                                    \
    do                                                               \
    {                                                                \
        if (g_io_trace_on && (g_io_trace_idx < PSX_IO_TRACE_SIZE))    \
        {                                                            \
            psx_io_trace_t *e = &g_io_trace[g_io_trace_idx++];        \
            e->addr = (a);                                           \
            e->value = (v);                                          \
            e->write = (w);                                          \
            e->size = (sz);                                          \
        }                                                            \
    } while (0)

void psx_prof_init(void);
void psx_prof_tick(void); /* prints a report once per second */

#define PROF_T0(name) uint32_t name = DWT->CYCCNT
#define PROF_ADD(field, t0) g_prof.field += DWT->CYCCNT - (t0)
#define PROF_INC(field) (g_prof.field++)

#else

#define PROF_T0(name) do {} while (0)
#define PROF_ADD(field, t0) do {} while (0)
#define PROF_INC(field) do {} while (0)
#define psx_prof_init() do {} while (0)
#define psx_prof_tick() do {} while (0)
#define PSX_IO_TRACE(a, v, w, sz) do {} while (0)

#endif

#endif
