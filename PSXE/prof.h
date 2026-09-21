#ifndef PSX_PROF_H
#define PSX_PROF_H

/* Lightweight DWT-based profiler for the PSX emulator hot loop.
   Set PSX_PROFILE to 0 to compile out completely. */

#include "prof_switch.h"

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
    uint32_t mdec_idct;/* core cycles in MDEC block decode + IDCT */
    uint32_t mdec_yuv; /* core cycles in MDEC colour conversion */
    uint32_t mdec_blk; /* MDEC blocks decoded */
    uint32_t jit_cmp;  /* core cycles translating blocks (part of cpu) */
    uint32_t jit_tier; /* core cycles revising the ITCM code tier (part of cpu) */
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

    /* rasterizer by primitive class: core cycles, primitives, bounding box
       pixels. 0 culled, 1 flat, 2 flat blended, 3 textured (paletted, fast
       loop), 4 textured (15 bit or raw), 5 gouraud, 6 textured gouraud,
       7 rectangle */
    uint32_t gte_cyc;  /* core cycles in GTE commands (part of cpu) */
    uint32_t gte_cnt;  /* GTE commands */
    uint32_t gte_mov;  /* GTE register moves and LWC2 / SWC2 */
    uint32_t dirty_flip; /* the picture went out of date: display window moved   */
    uint32_t dirty_draw; /* ... something was drawn or copied into it            */
    uint32_t ras_cyc[8];
    uint32_t ras_cnt[8];
    uint32_t ras_px[8];
    uint32_t t_start;  /* CYCCNT at window start */
} psx_prof_t;

extern psx_prof_t g_prof;

/* Continuously recorded ring buffer of I/O accesses, dumped when the emulated
   machine stops producing frames. Consecutive accesses to the same register are
   collapsed into a repeat count so a polling loop cannot flush out the history
   that led to the lockup. */
#define PSX_IO_TRACE_SIZE 384

typedef struct
{
    uint32_t addr;
    uint32_t value;
    uint32_t repeats;
    uint8_t write;
    uint8_t size;
} psx_io_trace_t;

extern psx_io_trace_t g_io_trace[PSX_IO_TRACE_SIZE];
extern volatile uint32_t g_io_trace_idx; /* next slot to write */
extern volatile uint32_t g_io_trace_len; /* number of valid entries */
extern volatile int32_t g_io_trace_on;

void psx_io_trace_record(uint32_t addr, uint32_t value, uint32_t write, uint32_t size);
void psx_io_trace_request_dump(void);
void psx_io_trace_dump(const char *tag);

#define PSX_IO_TRACE(a, v, w, sz)                                    \
    do                                                               \
    {                                                                \
        if (g_io_trace_on)                                           \
            psx_io_trace_record((a), (v), (w), (sz));                 \
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
#define psx_io_trace_request_dump() do {} while (0)

#endif

#endif
