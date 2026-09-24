#include "psx.h"
#include "prof.h"
#include "jit/jit.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
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

/*
    PC sampler. The cycle counters above say how long the emulator spends in the
    parts that were instrumented; this says where the core actually is, a
    thousand times a second, from the tick: by memory region - which is what
    tells translated code running from the ITCM from translated code running
    from SDRAM - and, for the two code RAMs, in 256 byte buckets that a script
    turns into function names.

    The tick interrupts a task, so the interrupted pc is in the frame on the
    process stack. (When it interrupts another handler instead, that is still
    the task's frame: the sample goes to what the task was doing.)
*/
enum
{
    PCS_JIT_FAST, /* translated code, ITCM tier  */
    PCS_JIT_SLOW, /* translated code, SDRAM      */
    PCS_ITCM,     /* dispatcher, helpers, ...    */
    PCS_OCRAM,    /* interpreter, GTE, GPU       */
    PCS_SDRAM,    /* anything else in SDRAM      */
    PCS_FLASH,
    PCS_OTHER,
    PCS_REGIONS
};

#define PCS_SHIFT 8
#define PCS_ITCM_BUCKETS (0x20000u >> PCS_SHIFT)
#define PCS_OCRAM_BUCKETS (0x40000u >> PCS_SHIFT)
#define PCS_FLASH_BUCKETS (0x80000u >> PCS_SHIFT) /* the image is about 520 KB */
#define PCS_BUCKETS (PCS_ITCM_BUCKETS + PCS_OCRAM_BUCKETS + PCS_FLASH_BUCKETS)

static volatile uint32_t g_pcs_region[PCS_REGIONS];
static uint32_t g_pcs_range[4];
static uint16_t __attribute__((section(".bss.$BOARD_SDRAM"))) g_pcs_hist[PCS_BUCKETS];

/* translated code running from SDRAM, by where in the master area: how much
   code would the fast tier have to hold to take most of it over? */
#define PCS_SLOW_BUCKETS (0x400000u >> PCS_SHIFT)

static uint16_t __attribute__((section(".bss.$BOARD_SDRAM"))) g_pcs_slow[PCS_SLOW_BUCKETS];

void __attribute__((section(".ramfunc.$SRAM_ITC"))) vApplicationTickHook(void)
{
    const uint32_t pc = ((const uint32_t *)__get_PSP())[6];

    uint32_t region;

    if (pc < 0x00020000u)
    {
        region = ((pc >= g_pcs_range[0]) && (pc < g_pcs_range[1])) ? PCS_JIT_FAST : PCS_ITCM;

        if (region == PCS_ITCM)
            g_pcs_hist[pc >> PCS_SHIFT]++;
    }
    else if ((pc >= 0x20200000u) && (pc < 0x20240000u))
    {
        region = PCS_OCRAM;
        g_pcs_hist[PCS_ITCM_BUCKETS + ((pc - 0x20200000u) >> PCS_SHIFT)]++;
    }
    else if ((pc >= g_pcs_range[2]) && (pc < g_pcs_range[3]))
    {
        region = PCS_JIT_SLOW;

        const uint32_t b = (pc - g_pcs_range[2]) >> PCS_SHIFT;

        if (b < PCS_SLOW_BUCKETS)
            g_pcs_slow[b]++;
    }
    else if ((pc >= 0x80000000u) && (pc < 0x82000000u))
        region = PCS_SDRAM;
    else if ((pc >= 0x60000000u) && (pc < 0x64000000u))
    {
        region = PCS_FLASH;

        if (pc < 0x60080000u)
            g_pcs_hist[PCS_ITCM_BUCKETS + PCS_OCRAM_BUCKETS + ((pc - 0x60000000u) >> PCS_SHIFT)]++;
    }
    else
        region = PCS_OTHER;

    g_pcs_region[region]++;
}

/* once a second: the regions; every 16th time the busiest buckets as well */
static void psx_pcs_report(void)
{
    static uint32_t round = 0;

    if (!g_pcs_range[1])
        psx_jit_code_ranges(g_pcs_range);

    PRINTF("PROF-PC jitfast=%u jitslow=%u itcm=%u ocram=%u sdram=%u flash=%u other=%u\r\n",
           (unsigned)g_pcs_region[PCS_JIT_FAST], (unsigned)g_pcs_region[PCS_JIT_SLOW],
           (unsigned)g_pcs_region[PCS_ITCM], (unsigned)g_pcs_region[PCS_OCRAM],
           (unsigned)g_pcs_region[PCS_SDRAM], (unsigned)g_pcs_region[PCS_FLASH],
           (unsigned)g_pcs_region[PCS_OTHER]);

    for (uint32_t i = 0; i < PCS_REGIONS; i++)
        g_pcs_region[i] = 0;

    if ((++round & 15u) != 0u)
        return;

    /* the slow tier: buckets (256 bytes of code each) it takes to cover half,
       80% and 95% of the samples, busiest first */
    {
        uint32_t total = 0, used = 0;

        for (uint32_t i = 0; i < PCS_SLOW_BUCKETS; i++)
        {
            total += g_pcs_slow[i];
            used += g_pcs_slow[i] ? 1u : 0u;
        }

        uint32_t acc = 0, n = 0, n50 = 0, n80 = 0, n95 = 0;

        while (total && (acc * 100u < total * 95u))
        {
            uint32_t best = 0, at = 0;

            for (uint32_t i = 0; i < PCS_SLOW_BUCKETS; i++)
            {
                if (g_pcs_slow[i] > best)
                {
                    best = g_pcs_slow[i];
                    at = i;
                }
            }

            g_pcs_slow[at] = 0;
            acc += best;
            n++;

            if (!n50 && (acc * 100u >= total * 50u))
                n50 = n;

            if (!n80 && (acc * 100u >= total * 80u))
                n80 = n;

            n95 = n;
        }

        PRINTF("PROF-PCSLOW samples=%u buckets=%u half=%u p80=%u p95=%u\r\n", (unsigned)total, (unsigned)used,
               (unsigned)n50, (unsigned)n80, (unsigned)n95);

        for (uint32_t i = 0; i < PCS_SLOW_BUCKETS; i++)
            g_pcs_slow[i] = 0;
    }

    PRINTF("PROF-PCTOP");

    for (uint32_t n = 0; n < 110u; n++)
    {
        uint32_t best = 0, at = 0;

        for (uint32_t i = 0; i < PCS_BUCKETS; i++)
        {
            if (g_pcs_hist[i] > best)
            {
                best = g_pcs_hist[i];
                at = i;
            }
        }

        if (!best)
            break;

        g_pcs_hist[at] = 0;

        PRINTF(" %x=%u",
               (unsigned)((at < PCS_ITCM_BUCKETS)
                              ? (at << PCS_SHIFT)
                              : ((at < (PCS_ITCM_BUCKETS + PCS_OCRAM_BUCKETS))
                                     ? (0x20200000u + ((at - PCS_ITCM_BUCKETS) << PCS_SHIFT))
                                     : (0x60000000u + ((at - PCS_ITCM_BUCKETS - PCS_OCRAM_BUCKETS) << PCS_SHIFT)))),
               (unsigned)best);
    }

    PRINTF("\r\n");

    for (uint32_t i = 0; i < PCS_BUCKETS; i++)
        g_pcs_hist[i] = 0;
}

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

    psx_pcs_report();

    {
        static const char *const ras_name[8] = {"cull", "flat", "flatT", "tex", "texS", "gour", "texG", "rect"};

        PRINTF("PROF-RAS");

        for (uint32_t k = 0; k < 8u; k++)
            PRINTF(" %s=%u/%u/%u", ras_name[k], (unsigned)g_prof.ras_cyc[k], (unsigned)g_prof.ras_cnt[k],
                   (unsigned)g_prof.ras_px[k]);

        PRINTF(" (cycles/prims/bbox px)\r\n");
    }

    PRINTF("PROF-MDEC idct=%u yuv=%u blocks=%u | PROF-JIT compile=%u retier=%u | PROF-GTE cyc=%u cmds=%u moves=%u | PROF-DIRTY flip=%u draw=%u\r\n",
           (unsigned)g_prof.mdec_idct, (unsigned)g_prof.mdec_yuv, (unsigned)g_prof.mdec_blk,
           (unsigned)g_prof.jit_cmp, (unsigned)g_prof.jit_tier,
           (unsigned)g_prof.gte_cyc, (unsigned)g_prof.gte_cnt, (unsigned)g_prof.gte_mov,
           (unsigned)g_prof.dirty_flip, (unsigned)g_prof.dirty_draw);

    PRINTF("PROF inst=%u ecyc=%u | cpu=%u gp0=%u dma=%u | dev=%u (cd=%u gpu=%u pad=%u tmr=%u dma=%u spu=%u) blit=%u bwait=%u | other=%u | frames=%u gp0cmds=%u px=%u (f=%u s=%u t4=%u t8=%u t15=%u r=%u fast=%u tr=%u raw=%u) | elapsed=%u\r\n",
           g_prof.instr, g_prof.ecycles,
           g_prof.cpu, g_prof.gp0, g_prof.dmax,
           g_prof.dev, g_prof.d_cdrom, g_prof.d_gpu, g_prof.d_pad, g_prof.d_timer, g_prof.d_dma, g_prof.d_spu,
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

/*
    Emulated CPU cycles between two device update rounds.

    A round costs a couple of hundred core cycles whether or not any device has
    anything to do, and at a fixed 64 cycle slice there were half a million of
    them per emulated second, nearly all of them finding nothing. Every device
    already knows when it next has to act - the GPU its next hblank edge, the
    timers their next target or wrap, the drive, the controller port and the
    MDEC DMA their delays - so the slice is sized to end there instead:

      - never shorter than PSX_DEV_SLICE_MIN, the old fixed slice: a deadline
        closer than that is served as late as it always was;
      - never longer than PSX_DEV_SLICE_MAX, which bounds how stale a register
        read can be (a timer counter, say) and how early a delay that was armed
        in the middle of a slice can fire, since it is charged the whole slice.
        It also stays well below the hblank window (853 GPU cycles, about 538 CPU
        cycles), so no edge can be stepped over;
      - planned again whenever the guest writes a device register, because that
        is how a nearer deadline comes about; the recompiler returns right
        after such a write.

    A block may overshoot the slice by one block's worth of cycles.
*/
#define PSX_DEV_SLICE_MIN 64u
#define PSX_DEV_SLICE_MAX 256u
#define PSX_DEV_SLICE_MAX_STEPS 32

static inline uint32_t __attribute__((always_inline)) psx_cycles_to_event(const psx_t *psx)
{
    uint32_t n = PSX_DEV_SLICE_MAX;
    uint32_t c;

    c = psx_gpu_cycles_to_edge(psx->gpu);

    if (c < n)
        n = c;

    const psx_timer_t *const timer = psx->timer;

    c = (timer->pending_cycles < timer->deadline_cycles)
            ? (uint32_t)(timer->deadline_cycles - timer->pending_cycles)
            : 0u;

    if (c < n)
        n = c;

    const psx_cdrom_t *const cd = psx->cdrom;

    if (cd->delay > 0)
    {
        if ((uint32_t)cd->delay < n)
            n = (uint32_t)cd->delay;
    }
    else if ((cd->state != CD_STATE_IDLE) && (cd->state != CD_STATE_PLAY))
    {
        n = 0; /* acts on the very next update */
    }

    if ((psx->pad->cycles_until_irq > 0) && ((uint32_t)psx->pad->cycles_until_irq < n))
        n = (uint32_t)psx->pad->cycles_until_irq;

    const psx_dma_t *const dma = psx->dma;

    if (dma->cdrom_irq_delay | dma->spu_irq_delay | dma->gpu_irq_delay | dma->otc_irq_delay)
    {
        n = 0; /* completion flags are raised by the next update */
    }
    else
    {
        /* counted in units of the original 21 cycle slice, see psx_dma_update */
        if (dma->mdec_in_irq_delay && ((dma->mdec_in_irq_delay * 21u) < n))
            n = dma->mdec_in_irq_delay * 21u;

        if (dma->mdec_out_irq_delay && ((dma->mdec_out_irq_delay * 21u) < n))
            n = dma->mdec_out_irq_delay * 21u;
    }

    return (n < PSX_DEV_SLICE_MIN) ? PSX_DEV_SLICE_MIN : n;
}

#if PSXE_SOUND == 2
/*
    The sound: every 768 emulated CPU cycles (33.8688 MHz / 44.1 kHz) one stereo
    sample of the SPU's voices mixed with the CD's (XA or CD audio), made 32 at
    a time and handed to the output. Making them also runs the voices -
    envelopes, ENDX, the SPU interrupt - which psx_spu_update does silently when
    nothing plays them (PSXE_SOUND 1).

    32 (0.73 ms): the SPU mixes a batch voice by voice, and what it costs per
    batch - every voice's state loaded once, the reverb's cold cache lines - is
    spread over twice the samples of the 16 it was. The voices' state (ENDX,
    envelopes, the IRQ) still moves every 0.73 ms, where a game looks at it once
    a frame.
*/
#define PSX_AUDIO_CYCLES 768u
#define PSX_AUDIO_BATCH 32u

static inline int16_t psx_clamp16(int32_t v)
{
    return (int16_t)((v < -32768) ? -32768 : ((v > 32767) ? 32767 : v));
}

static void __attribute__((noinline, section(".ramfunc.$SRAM_OC"))) psx_audio_batch(psx_t *psx)
{
    int16_t buf[2u * PSX_AUDIO_BATCH];
    uint32_t spu[PSX_AUDIO_BATCH];

    memset(buf, 0, sizeof(buf));
    psx_cdrom_get_audio_samples(psx->cdrom, buf, sizeof(buf));
    psx_spu_get_samples(psx->spu, spu, PSX_AUDIO_BATCH);

    for (uint32_t i = 0; i < PSX_AUDIO_BATCH; i++)
    {
        const uint32_t s = spu[i];

        buf[2u * i] = psx_clamp16((int32_t)buf[2u * i] + (int16_t)(s & 0xffffu));
        buf[2u * i + 1u] = psx_clamp16((int32_t)buf[2u * i + 1u] + (int16_t)(s >> 16));
    }

    psx_platform_audio_out(buf, PSX_AUDIO_BATCH);
}

static inline __attribute__((always_inline)) void psx_audio_update(psx_t *psx, uint32_t cycles)
{
    psx->audio_acc += cycles;

    while (psx->audio_acc >= (PSX_AUDIO_CYCLES * PSX_AUDIO_BATCH))
    {
        psx->audio_acc -= PSX_AUDIO_CYCLES * PSX_AUDIO_BATCH;

        psx_audio_batch(psx);
    }
}
#endif

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_update(psx_t *psx)
{
    psx_cpu_t *const cpu = psx->cpu;
    uint32_t acc = 0;
    uint32_t steps = 0;

    PROF_T0(t_cpu);

    uint32_t limit = psx_cycles_to_event(psx);

    g_psx_bus_io_written = 0;

    do
    {
#if PSX_JIT_ENABLE
        /* Recompiled blocks, handing over to one another for as long as the
           slice lasts; instructions the translator cannot emit natively call
           back into the interpreter from inside the block. */
        acc += psx_jit_step(cpu, limit - acc);
#else
        psx_cpu_cycle(cpu);

        acc += cpu->last_cycles;
#endif
        steps++;

        if (g_psx_bus_io_written)
        {
            g_psx_bus_io_written = 0;

            /* Both are measured from the last round: the devices have not been
               told about this slice's cycles yet. */
            const uint32_t again = psx_cycles_to_event(psx);

            if (again < limit)
                limit = again;
        }
    }
    while ((acc < limit) && (steps < PSX_DEV_SLICE_MAX_STEPS));

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
    /* DMA delays are counted in rounds of the original 21 cycle slice */
    psx_dma_update(psx->dma, (int32_t)((acc + 20u) / 21u));
    PROF_ADD(d_dma, t_d5);

#if PSXE_SOUND == 1
    PROF_T0(t_d6);
    /* the voices run although nothing plays them: games wait on them */
    psx_spu_update(psx->spu, acc);
    PROF_ADD(d_spu, t_d6);
#elif PSXE_SOUND == 2
    PROF_T0(t_d6);
    psx_audio_update(psx, acc);
    PROF_ADD(d_spu, t_d6);
#endif

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
    /* The 368 pixel mode is 368 pixels. This used to say 384, and the sixteen
       columns too many are whatever lies in VRAM right of the picture: Tekken 3,
       which keeps its textures there, had a strip of them down the right edge. */
    return psx_get_dmode_width(psx);
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
    (void)udata;

    psx_tty_putchar(c);
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
