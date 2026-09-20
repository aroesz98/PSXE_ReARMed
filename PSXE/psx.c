#include "psx.h"
#include "prof.h"
#include "jit/jit.h"
#include <stdlib.h>
#include <stdint.h>
#include "fsl_debug_console.h"
#include "core_cm7.h"
#include "FreeRTOS.h"
#include "task.h"

// Static buffer for PSX instance
psx_t g_psx_instance;
static int32_t g_psx_instance_used = 0;

psx_t *psx_create(void)
{
    if (g_psx_instance_used)
    {
        return NULL; // Only one instance allowed
    }
    g_psx_instance_used = 1;
    return &g_psx_instance;
}

int32_t psx_load_bios(psx_t *psx, const char *path)
{
    return psx_bios_load(psx->bios, path);
}

void psx_load_state(psx_t *psx, const char *path)
{
    log_fatal("State saving/loading is not yet supported");

    exit(1);
}

void psx_save_state(psx_t *psx, const char *path)
{
    log_fatal("State saving/loading is not yet supported");

    exit(1);
}

void psx_load_exe(psx_t *psx, const char *path)
{
    psx_exe_load(psx->cpu, path);
}

uint32_t last_update_time = 0;
uint32_t loop_cnt = 0;
volatile uint32_t g_cycles_wm = 0;
uint32_t frame_count = 0;

// #define PSX_DEBUG_PERFORMANCE

#if PSX_PROFILE
psx_prof_t g_prof;

psx_io_trace_t __attribute__((section(".bss.$SRAM_DTC"))) g_io_trace[PSX_IO_TRACE_SIZE];
volatile uint32_t g_io_trace_idx = 0;
volatile uint32_t g_io_trace_len = 0;
volatile int32_t g_io_trace_on = 1; /* record from the start */

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_io_trace_record(uint32_t addr, uint32_t value,
                                                                       uint32_t write, uint32_t size)
{
    /* Only the registers that matter for interrupt / DMA / CD sequencing.
       SPU FIFO and GP0 data would otherwise flood the ring buffer. */
    const uint32_t off = addr - 0x1f801000u;

    /* 0xF00000xx / 0xF10000xx are internal markers (interrupt controller and
       DMA engine events), they always pass. */
    if ((off >= 0x1000u) && (addr < 0xF0000000u))
        return;

    const int interesting = (addr >= 0xF0000000u) ||
        ((off >= 0x070u) && (off < 0x078u)) ||  /* I_STAT / I_MASK          */
        ((off >= 0x080u) && (off < 0x100u)) ||  /* DMA channels + DPCR/DICR */
        ((off >= 0x100u) && (off < 0x130u)) ||  /* timers                   */
        ((off >= 0x800u) && (off < 0x804u)) ||  /* CDROM                    */
        ((off >= 0x810u) && (off < 0x818u));    /* GPU (GP0/GP1/GPUSTAT)    */

    if (!interesting)
        return;

    uint32_t idx = g_io_trace_idx;

    /* collapse a polling loop into one entry (value of the last access wins) */
    if (g_io_trace_len)
    {
        psx_io_trace_t *last = &g_io_trace[(idx + PSX_IO_TRACE_SIZE - 1) % PSX_IO_TRACE_SIZE];

        if ((last->addr == addr) && (last->write == write))
        {
            last->repeats++;
            last->value = value;
            return;
        }
    }

    psx_io_trace_t *e = &g_io_trace[idx];

    e->addr = addr;
    e->value = value;
    e->repeats = 0;
    e->write = (uint8_t)write;
    e->size = (uint8_t)size;

    g_io_trace_idx = (idx + 1u) % PSX_IO_TRACE_SIZE;

    if (g_io_trace_len < PSX_IO_TRACE_SIZE)
        g_io_trace_len++;
}

void psx_prof_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    memset(&g_prof, 0, sizeof(g_prof));
    g_prof.t_start = DWT->CYCCNT;
}

static volatile int32_t g_io_trace_dump_req = 0;
static volatile int32_t g_io_trace_dumped_once = 0;

void psx_io_trace_request_dump(void)
{
    if (!g_io_trace_dumped_once)
    {
        g_io_trace_dumped_once = 1;
        g_io_trace_dump_req = 1;
    }
}

void psx_io_trace_dump(const char *tag)
{
    g_io_trace_on = 0;

    uint32_t len = g_io_trace_len;
    uint32_t start = (g_io_trace_idx + PSX_IO_TRACE_SIZE - len) % PSX_IO_TRACE_SIZE;

    PRINTF("IOTRACE-RING [%s] %u entries (oldest first):\r\n", tag, (unsigned)len);

    for (uint32_t i = 0; i < len; i++)
    {
        const psx_io_trace_t *e = &g_io_trace[(start + i) % PSX_IO_TRACE_SIZE];

        PRINTF("  %c%u %08x = %08x x%u\r\n",
               e->write ? 'W' : 'R',
               (unsigned)e->size,
               (unsigned)e->addr,
               (unsigned)e->value,
               (unsigned)(e->repeats + 1u));
    }

    PRINTF("IOTRACE-END\r\n");

    g_io_trace_len = 0;
    g_io_trace_idx = 0;
    g_io_trace_on = 1;
}

void psx_prof_tick(void)
{
    if (g_io_trace_dump_req)
    {
        g_io_trace_dump_req = 0;
        psx_io_trace_dump("LOST DMA IRQ");
    }

    uint32_t now = DWT->CYCCNT;
    uint32_t elapsed = now - g_prof.t_start;

    if (elapsed < SystemCoreClock)
        return;

    uint32_t other = elapsed - g_prof.cpu - g_prof.dev;

    PRINTF("PROF inst=%u ecyc=%u | cpu=%u gp0=%u dma=%u | dev=%u (cd=%u gpu=%u pad=%u tmr=%u dma=%u) blit=%u bwait=%u | other=%u | frames=%u gp0cmds=%u px=%u (f=%u s=%u t4=%u t8=%u t15=%u r=%u fast=%u tr=%u raw=%u) | elapsed=%u\r\n",
           g_prof.instr, g_prof.ecycles,
           g_prof.cpu, g_prof.gp0, g_prof.dmax,
           g_prof.dev, g_prof.d_cdrom, g_prof.d_gpu, g_prof.d_pad, g_prof.d_timer, g_prof.d_dma,
           g_prof.blit, g_prof.bwait,
           other, g_prof.frames, g_prof.gp0cmds, g_prof.pixels,
           g_prof.px_flat, g_prof.px_shade, g_prof.px_t4, g_prof.px_t8, g_prof.px_t15, g_prof.px_rect,
           g_prof.px_fast, g_prof.px_transp, g_prof.px_raw,
           elapsed);

    /* State snapshot: makes it obvious when the emulated machine is stuck
       spinning somewhere instead of making progress. */
    {
        psx_t *p = &g_psx_instance;

        PRINTF("STATE pc=%08x sr=%08x cause=%08x | ic stat=%04x mask=%04x | gpustat=%08x line=%d dirty=%d | cdrom st=%d dly=%d ifr=%02x ier=%02x | dicr=%08x dpcr=%08x\r\n",
               (unsigned)p->cpu->pc, (unsigned)p->cpu->cop0_r[COP0_SR], (unsigned)p->cpu->cop0_r[COP0_CAUSE],
               (unsigned)p->ic->stat, (unsigned)p->ic->mask,
               (unsigned)p->gpu->gpustat, (int)p->gpu->line, (int)p->gpu->vram_dirty,
               (int)p->cdrom->state, (int)p->cdrom->delay, (unsigned)p->cdrom->ifr, (unsigned)p->cdrom->ier,
               (unsigned)p->dma->dicr, (unsigned)p->dma->dpcr);

        PRINTF("TIMERS t0=%d/%u clk=%d pause=%d sync=%d/%d | t1=%d/%u clk=%d pause=%d sync=%d/%d irq=%d/%d | t2=%d/%u clk=%d pause=%d | hbl=%d vbl=%d\r\n",
               (int)p->timer->timer[0].counter, (unsigned)p->timer->timer[0].target,
               (int)p->timer->timer[0].clk_source, (int)p->timer->timer[0].paused,
               (int)p->timer->timer[0].sync_enable, (int)p->timer->timer[0].sync_mode,
               (int)p->timer->timer[1].counter, (unsigned)p->timer->timer[1].target,
               (int)p->timer->timer[1].clk_source, (int)p->timer->timer[1].paused,
               (int)p->timer->timer[1].sync_enable, (int)p->timer->timer[1].sync_mode,
               (int)p->timer->timer[1].irq_target, (int)p->timer->timer[1].irq_max,
               (int)p->timer->timer[2].counter, (unsigned)p->timer->timer[2].target,
               (int)p->timer->timer[2].clk_source, (int)p->timer->timer[2].paused,
               (int)p->timer->hblank, (int)p->timer->vblank);
    }

    /* Stuck detection: arm an I/O trace after a few seconds without any
       rendering, dump it on the next tick. */
    {
        static uint32_t idle_seconds = 0;

        if ((g_prof.frames == 0) && (g_prof.gp0cmds == 0))
            idle_seconds++;
        else
            idle_seconds = 0;

        if (idle_seconds == 4)
        {
            /* Freeze and dump the history that led here, oldest first */
            g_io_trace_on = 0;

            uint32_t len = g_io_trace_len;
            uint32_t start = (g_io_trace_idx + PSX_IO_TRACE_SIZE - len) % PSX_IO_TRACE_SIZE;

            PRINTF("IOTRACE-RING %u entries (oldest first):\r\n", (unsigned)len);

            for (uint32_t i = 0; i < len; i++)
            {
                const psx_io_trace_t *e = &g_io_trace[(start + i) % PSX_IO_TRACE_SIZE];

                PRINTF("  %c%u %08x = %08x x%u\r\n",
                       e->write ? 'W' : 'R',
                       (unsigned)e->size,
                       (unsigned)e->addr,
                       (unsigned)e->value,
                       (unsigned)(e->repeats + 1u));
            }

            PRINTF("IOTRACE-END\r\n");

            g_io_trace_len = 0;
            g_io_trace_idx = 0;
            g_io_trace_on = 1;
            idle_seconds = 0;
        }
    }

    memset(&g_prof, 0, sizeof(g_prof));
    g_prof.t_start = DWT->CYCCNT;
}
#endif

/* Emulated CPU cycles executed between two device update rounds.
   The devices only need ~scanline resolution; updating them after every
   single instruction cost more than the interpreter itself.
   Must stay well below the GPU hblank window (~54 CPU cycles) so no
   hblank/vblank edge can be stepped over. */
#define PSX_DEV_SLICE_CYCLES 21
#define PSX_DEV_SLICE_MAX_STEPS 32

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_update(psx_t *psx)
{
    psx_cpu_t *const cpu = psx->cpu;
    uint32_t acc = 0;
    uint32_t steps = 0;

    PROF_T0(t_cpu);

    do
    {
#if PSX_JIT_ENABLE
        /* One recompiled block; instructions the translator cannot emit
           natively call back into the interpreter from inside the block. */
        acc += psx_jit_step(cpu);
#else
        psx_cpu_cycle(cpu);

        acc += cpu->last_cycles;
#endif
        steps++;
    }
    while ((acc < PSX_DEV_SLICE_CYCLES) && (steps < PSX_DEV_SLICE_MAX_STEPS));

    PROF_ADD(cpu, t_cpu);

#if PSX_PROFILE
    g_prof.instr += steps;
    g_prof.ecycles += acc;
#endif

    PROF_T0(t_dev);

    /* Guards for the devices that are idle most of the time: skipping the call
       (and its prologue) is much cheaper than entering it just to return. */
    PROF_T0(t_d1);
    if ((psx->cdrom->delay > 0) || (psx->cdrom->state != CD_STATE_IDLE))
        psx_cdrom_update(psx->cdrom, acc);
    PROF_ADD(d_cdrom, t_d1);

    PROF_T0(t_d2);
    psx_gpu_update(psx->gpu, acc);
    PROF_ADD(d_gpu, t_d2);

    PROF_T0(t_d3);
    if (psx->pad->cycles_until_irq)
        psx_pad_update(psx->pad, acc);
    PROF_ADD(d_pad, t_d3);

    PROF_T0(t_d4);
    psx_timer_update(psx->timer, acc);
    PROF_ADD(d_timer, t_d4);

    PROF_T0(t_d5);
    /* DMA delays are counted in update rounds, not in cycles */
    psx_dma_update(psx->dma, steps);
    PROF_ADD(d_dma, t_d5);

    PROF_ADD(dev, t_dev);

    psx_prof_tick();
}

void *psx_get_display_buffer(psx_t *psx)
{
    return psx_gpu_get_display_buffer(psx->gpu);
}

void *psx_get_vram(psx_t *psx)
{
    return psx->gpu->vram;
}

uint32_t psx_get_display_width(psx_t *psx)
{
    int32_t width = psx_get_dmode_width(psx);

    if (width == 368)
        width = 384;

    return width;
}

uint32_t psx_get_display_height(psx_t *psx)
{
    return psx_get_dmode_height(psx);
}

uint32_t psx_get_display_format(psx_t *psx)
{
    return (psx->gpu->display_mode >> 4) & 1;
}

uint32_t psx_get_dmode_width(psx_t *psx)
{
    static int32_t dmode_hres_table[] = {
        256, 320, 512, 640};

    if (psx->gpu->display_mode & 0x40)
    {
        return 368;
    }
    else
    {
        return dmode_hres_table[psx->gpu->display_mode & 0x3];
    }
}

uint32_t psx_get_dmode_height(psx_t *psx)
{
    /* The vertical resolution bit only means 480 lines together with vertical
       interlace; on its own the hardware still shows 240, and the real height
       then comes from the vertical display range (that is where 224 line modes
       come from). */
    if ((psx->gpu->display_mode & 0x4) && (psx->gpu->display_mode & 0x20))
        return 480;

    int32_t disp = psx->gpu->disp_y2 - psx->gpu->disp_y1;

    if (disp < (255 - 16))
        return disp;

    return 240;
}

float psx_get_display_aspect(psx_t *psx)
{
    float width = psx_get_dmode_width(psx);
    float height = psx_get_dmode_height(psx);

    if (height > width)
        return 4.0 / 3.0;

    float aspect = width / height;

    if (aspect > (4.0 / 3.0))
        return 4.0 / 3.0;

    return aspect;
}

void atcons_tx(void *udata, unsigned char c)
{
    putchar(c);
}

int32_t psx_init(psx_t *psx, const char *bios_path, const char *exp_path)
{
    memset(psx, 0, sizeof(psx_t));

    psx->bios = psx_bios_create();
    psx->ram = psx_ram_create();
    psx->dma = psx_dma_create();
    psx->exp1 = psx_exp1_create();
    psx->exp2 = psx_exp2_create();
    psx->mc1 = psx_mc1_create();
    psx->mc2 = psx_mc2_create();
    psx->mc3 = psx_mc3_create();
    psx->ic = psx_ic_create();
    psx->scratchpad = psx_scratchpad_create();
    psx->gpu = psx_gpu_create();
    psx->spu = psx_spu_create();
    psx->bus = psx_bus_create();
    psx->cpu = psx_cpu_create();
    psx->timer = psx_timer_create();
    psx->cdrom = psx_cdrom_create();
    psx->pad = psx_pad_create();
    psx->mdec = psx_mdec_create();

    psx_bus_init(psx->bus);

    psx_bus_init_bios(psx->bus, psx->bios);
    psx_bus_init_ram(psx->bus, psx->ram);
    psx_bus_init_dma(psx->bus, psx->dma);
    psx_bus_init_exp1(psx->bus, psx->exp1);
    psx_bus_init_exp2(psx->bus, psx->exp2);
    psx_bus_init_mc1(psx->bus, psx->mc1);
    psx_bus_init_mc2(psx->bus, psx->mc2);
    psx_bus_init_mc3(psx->bus, psx->mc3);
    psx_bus_init_ic(psx->bus, psx->ic);
    psx_bus_init_scratchpad(psx->bus, psx->scratchpad);
    psx_bus_init_gpu(psx->bus, psx->gpu);
    psx_bus_init_spu(psx->bus, psx->spu);
    psx_bus_init_timer(psx->bus, psx->timer);
    psx_bus_init_cdrom(psx->bus, psx->cdrom);
    psx_bus_init_pad(psx->bus, psx->pad);
    psx_bus_init_mdec(psx->bus, psx->mdec);

    // Init devices
    psx_bios_init(psx->bios);

    if (psx_bios_load(psx->bios, bios_path))
        return 1;

    psx_mc1_init(psx->mc1);
    psx_mc2_init(psx->mc2);
    psx_mc3_init(psx->mc3);
    psx_ram_init(psx->ram, psx->mc2, 0x200000);
    psx_dma_init(psx->dma, psx->bus, psx->ic);

    if (psx_exp1_init(psx->exp1, psx->mc1, exp_path))
        return 2;

    psx_exp2_init(psx->exp2, atcons_tx, NULL);
    psx_ic_init(psx->ic, psx->cpu);
    psx_scratchpad_init(psx->scratchpad);
    psx_gpu_init(psx->gpu, psx->ic);
    psx_spu_init(psx->spu, psx->ic);
    psx_timer_init(psx->timer, psx->ic, psx->gpu);
    psx_cdrom_init(psx->cdrom, psx->ic);
    psx_pad_init(psx->pad, psx->ic);
    psx_mdec_init(psx->mdec);
    psx_cpu_init(psx->cpu, psx->bus);

    psx_jit_init();

    return 0;
}

int32_t psx_load_expansion(psx_t *psx, const char *path)
{
    return psx_exp1_init(psx->exp1, psx->mc1, path);
}

void psx_hard_reset(psx_t *psx)
{
    log_fatal("Hard reset not yet implemented");

    exit(1);
}

void psx_soft_reset(psx_t *psx)
{
    psx_jit_reset();

    psx_cpu_init(psx->cpu, psx->bus);
}

uint32_t *psx_take_screenshot(psx_t *psx)
{
    log_fatal("Screenshots not yet supported");

    exit(1);
}

int32_t psx_swap_disc(psx_t *psx, const char *path)
{
    psx_cdrom_destroy(psx->cdrom);

    psx->cdrom = psx_cdrom_create();

    psx_bus_init_cdrom(psx->bus, psx->cdrom);

    psx_cdrom_init(psx->cdrom, psx->ic);

    return psx_cdrom_open(psx->cdrom, path);
}

void psx_destroy(psx_t *psx)
{
    psx_cpu_destroy(psx->cpu);
    psx_bios_destroy(psx->bios);
    psx_bus_destroy(psx->bus);
    psx_ram_destroy(psx->ram);
    psx_exp1_destroy(psx->exp1);
    psx_mc1_destroy(psx->mc1);
    psx_mc2_destroy(psx->mc2);
    psx_mc3_destroy(psx->mc3);
    psx_ic_destroy(psx->ic);
    psx_scratchpad_destroy(psx->scratchpad);
    psx_gpu_destroy(psx->gpu);
    psx_spu_destroy(psx->spu);
    psx_timer_destroy(psx->timer);
    psx_cdrom_destroy(psx->cdrom);
    psx_pad_destroy(psx->pad);
    psx_mdec_destroy(psx->mdec);

    // Mark instance as available again
    g_psx_instance_used = 0;
}

void psx_print_perf_stats(void)
{
    PRINTF("PSX Performance Statistics:\r\n");
    PRINTF("Performance data is printed every 1 million updates (~4.6 hours)\r\n");
    PRINTF("Use the real-time output during emulation for detailed timing\r\n");
}

psx_bios_t *psx_get_bios(psx_t *psx)
{
    return psx->bios;
}

psx_ram_t *psx_get_ram(psx_t *psx)
{
    return psx->ram;
}

psx_dma_t *psx_get_dma(psx_t *psx)
{
    return psx->dma;
}

psx_exp1_t *psx_get_exp1(psx_t *psx)
{
    return psx->exp1;
}

psx_exp2_t *psx_get_exp2(psx_t *psx)
{
    return psx->exp2;
}

psx_mc1_t *psx_get_mc1(psx_t *psx)
{
    return psx->mc1;
}

psx_mc2_t *psx_get_mc2(psx_t *psx)
{
    return psx->mc2;
}

psx_mc3_t *psx_get_mc3(psx_t *psx)
{
    return psx->mc3;
}

psx_ic_t *psx_get_ic(psx_t *psx)
{
    return psx->ic;
}

psx_scratchpad_t *psx_get_scratchpad(psx_t *psx)
{
    return psx->scratchpad;
}

psx_gpu_t *psx_get_gpu(psx_t *psx)
{
    return psx->gpu;
}

psx_spu_t *psx_get_spu(psx_t *psx)
{
    return psx->spu;
}

psx_bus_t *psx_get_bus(psx_t *psx)
{
    return psx->bus;
}

psx_timer_t *psx_get_timer(psx_t *psx)
{
    return psx->timer;
}

psx_cdrom_t *psx_get_cdrom(psx_t *psx)
{
    return psx->cdrom;
}

psx_pad_t *psx_get_pad(psx_t *psx)
{
    return psx->pad;
}

psx_mdec_t *psx_get_mdec(psx_t *psx)
{
    return psx->mdec;
}

psx_cpu_t *psx_get_cpu(psx_t *psx)
{
    return psx->cpu;
}
