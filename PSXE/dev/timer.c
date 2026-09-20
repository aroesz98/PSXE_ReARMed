#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "timer.h"
#include "../log.h"

#define T0_COUNTER timer->timer[0].counter
#define T0_SYNC_EN timer->timer[0].sync_enable
#define T0_SYNC_MODE timer->timer[0].sync_mode
#define T0_RESET_TGT timer->timer[0].reset_target
#define T0_IRQ_TGT timer->timer[0].irq_target
#define T0_IRQ_MAX timer->timer[0].irq_max
#define T0_IRQ_REPEAT timer->timer[0].irq_repeat
#define T0_IRQ_TOGGLE timer->timer[0].irq_toggle
#define T0_CLKSRC timer->timer[0].clk_source
#define T0_IRQ timer->timer[0].irq
#define T0_TGT_REACHED timer->timer[0].target_reached
#define T0_MAX_REACHED timer->timer[0].max_reached
#define T0_IRQ_FIRED timer->timer[0].irq_fired
#define T0_PAUSED timer->timer[0].paused
#define T0_BLANK_ONCE timer->timer[0].blank_once
#define T1_COUNTER timer->timer[1].counter
#define T1_SYNC_EN timer->timer[1].sync_enable
#define T1_SYNC_MODE timer->timer[1].sync_mode
#define T1_RESET_TGT timer->timer[1].reset_target
#define T1_IRQ_TGT timer->timer[1].irq_target
#define T1_IRQ_MAX timer->timer[1].irq_max
#define T1_IRQ_REPEAT timer->timer[1].irq_repeat
#define T1_IRQ_TOGGLE timer->timer[1].irq_toggle
#define T1_CLKSRC timer->timer[1].clk_source
#define T1_IRQ timer->timer[1].irq
#define T1_TGT_REACHED timer->timer[1].target_reached
#define T1_MAX_REACHED timer->timer[1].max_reached
#define T1_IRQ_FIRED timer->timer[1].irq_fired
#define T1_PAUSED timer->timer[1].paused
#define T1_BLANK_ONCE timer->timer[1].blank_once
#define T2_COUNTER timer->timer[2].counter
#define T2_SYNC_EN timer->timer[2].sync_enable
#define T2_SYNC_MODE timer->timer[2].sync_mode
#define T2_RESET_TGT timer->timer[2].reset_target
#define T2_IRQ_TGT timer->timer[2].irq_target
#define T2_IRQ_MAX timer->timer[2].irq_max
#define T2_IRQ_REPEAT timer->timer[2].irq_repeat
#define T2_IRQ_TOGGLE timer->timer[2].irq_toggle
#define T2_CLKSRC timer->timer[2].clk_source
#define T2_IRQ timer->timer[2].irq
#define T2_TGT_REACHED timer->timer[2].target_reached
#define T2_MAX_REACHED timer->timer[2].max_reached
#define T2_IRQ_FIRED timer->timer[2].irq_fired
#define T2_PAUSED timer->timer[2].paused
#define T2_BLANK_ONCE timer->timer[2].blank_once
#define T2_DIV_COUNTER timer->timer[2].div_counter

/* 16.16 fixed point increments of the fractional clock sources */
#define TIMER_FRAC_ONE 65536u

uint16_t timer_get_mode(psx_timer_t *timer, int32_t index)
{
    uint16_t value = (timer->timer[index].sync_enable << 0) |
                     (timer->timer[index].sync_mode << 1) |
                     (timer->timer[index].reset_target << 3) |
                     (timer->timer[index].irq_target << 4) |
                     (timer->timer[index].irq_max << 5) |
                     (timer->timer[index].irq_repeat << 6) |
                     (timer->timer[index].irq_toggle << 7) |
                     (timer->timer[index].clk_source << 8) |
                     (timer->timer[index].irq << 10) |
                     (timer->timer[index].target_reached << 11) |
                     (timer->timer[index].max_reached << 12);

    timer->timer[index].target_reached = 0;
    timer->timer[index].max_reached = 0;

    return value;
}

void timer_set_mode(psx_timer_t *timer, int32_t index, uint16_t value)
{
    timer->timer[index].sync_enable = (value >> 0) & 1;
    timer->timer[index].sync_mode = (value >> 1) & 3;
    timer->timer[index].reset_target = (value >> 3) & 1;
    timer->timer[index].irq_target = (value >> 4) & 1;
    timer->timer[index].irq_max = (value >> 5) & 1;
    timer->timer[index].irq_repeat = (value >> 6) & 1;
    timer->timer[index].irq_toggle = (value >> 7) & 1;
    timer->timer[index].clk_source = (value >> 8) & 3;
    timer->timer[index].target_reached = 0;
    timer->timer[index].max_reached = 0;

    // IRQ and counter are set to 0 on mode writes
    timer->timer[index].irq = 1;
    timer->timer[index].irq_fired = 0;
    timer->timer[index].counter = 0;
    timer->timer[index].counter_frac = 0;
    timer->timer[index].div_counter = 0;
    timer->timer[index].blank_once = 0;
    timer->timer[index].paused = 0;

    switch (index)
    {
    case 0:
    {
        if ((T0_SYNC_MODE == 1) || (T0_SYNC_MODE == 2) || !T0_SYNC_EN)
            return;

        T0_PAUSED = timer->hblank | (T0_SYNC_MODE == 3);
    }
    break;

    case 1:
    {
        if ((T1_SYNC_MODE == 1) || (T1_SYNC_MODE == 2) || !T1_SYNC_EN)
            return;

        T1_PAUSED = timer->vblank | (T1_SYNC_MODE == 3);
    }
    break;

    case 2:
    {
        if (!T2_SYNC_EN)
            return;

        T2_PAUSED = (T2_SYNC_MODE == 0) || (T2_SYNC_MODE == 3);
    }
    break;
    }
}

const char *g_psx_timer_reg_names[] = {
    "counter", 0, 0, 0,
    "mode", 0, 0, 0,
    "target", 0, 0, 0};

// Static buffer for timer instance
static psx_timer_t __attribute__((section(".bss.$SRAM_DTC"), aligned(4))) g_timer_instance;
static int32_t g_timer_instance_used = 0;

psx_timer_t *psx_timer_create(void)
{
    if (g_timer_instance_used)
    {
        return NULL; // Only one instance allowed
    }
    g_timer_instance_used = 1;
    return &g_timer_instance;
}

void psx_timer_init(psx_timer_t *timer, psx_ic_t *ic, psx_gpu_t *gpu)
{
    memset(timer, 0, sizeof(psx_timer_t));

    timer->io_base = PSX_TIMER_BEGIN;
    timer->io_size = PSX_TIMER_SIZE;

    timer->ic = ic;
    timer->gpu = gpu;
}

uint32_t psx_timer_read32(psx_timer_t *timer, uint32_t offset)
{
    psx_timer_flush(timer);

    int32_t index = offset >> 4;
    int32_t reg = offset & 0xf;

    switch (reg)
    {
    case 0:
        return timer->timer[index].counter;
    case 4:
        return timer_get_mode(timer, index);
    case 8:
        return timer->timer[index].target;
    }

    PRINTF("Unhandled 32-bit TIMER read at offset %08lx\n", offset);

    return 0x0;
}

uint16_t psx_timer_read16(psx_timer_t *timer, uint32_t offset)
{
    psx_timer_flush(timer);

    int32_t index = offset >> 4;
    int32_t reg = offset & 0xf;

    switch (reg)
    {
    case 0:
        return timer->timer[index].counter;
    case 4:
        return timer_get_mode(timer, index);
    case 8:
        return timer->timer[index].target;
    }

    PRINTF("Unhandled 16-bit TIMER read at offset %08lx\n", offset);

    return 0x0;
}

uint8_t psx_timer_read8(psx_timer_t *timer, uint32_t offset)
{
    PRINTF("Unhandled 8-bit TIMER read at offset %08lx\n", offset);

    return 0x0;
}

static inline void timer_handle_irq_inl(psx_timer_t *timer, int32_t i);

void psx_timer_write32(psx_timer_t *timer, uint32_t offset, uint32_t value)
{
    psx_timer_flush(timer);

    int32_t index = offset >> 4;
    int32_t reg = offset & 0xf;

    switch (reg)
    {
    case 0:
        timer->timer[index].counter = value & 0xffff;
        break;
    case 4:
        timer_set_mode(timer, index, value);
        break;
    case 8:
        timer->timer[index].target = value & 0xffff;
        break;
    }

    timer_handle_irq_inl(timer, index);
}

void psx_timer_write16(psx_timer_t *timer, uint32_t offset, uint16_t value)
{
    psx_timer_flush(timer);

    int32_t index = offset >> 4;
    int32_t reg = offset & 0xf;

    switch (reg)
    {
    case 0:
        timer->timer[index].counter = value & 0xffff;
        break;
    case 4:
        timer_set_mode(timer, index, value);
        break;
    case 8:
        timer->timer[index].target = value & 0xffff;
        break;
    }

    timer_handle_irq_inl(timer, index);
}

void psx_timer_write8(psx_timer_t *timer, uint32_t offset, uint8_t value)
{
    PRINTF("Unhandled 8-bit TIMER write at offset %08lx (%02x)\r\n", offset, value);
}

static inline void timer_handle_irq_inl(psx_timer_t *timer, int32_t i)
{
    int32_t irq = 0;

    int32_t target_reached = timer->timer[i].counter > timer->timer[i].target;
    int32_t max_reached = timer->timer[i].counter > 65535u;

    /* Nothing reached yet: this is by far the most common case and it must be
       cheap, this runs for three timers on every device update round. */
    if (!target_reached && !max_reached)
        return;

    /* No interrupt configured: only the status flags (and the optional target
       reset) can be affected, so skip the whole interrupt state machine. */
    if (!timer->timer[i].irq_target && !timer->timer[i].irq_max)
    {
        if (target_reached)
        {
            timer->timer[i].target_reached = 1;

            if (timer->timer[i].reset_target)
                timer->timer[i].counter = 0;
        }

        if (max_reached)
        {
            timer->timer[i].counter = 0;
            timer->timer[i].max_reached = 1;
        }

        return;
    }

    if (target_reached)
    {
        timer->timer[i].target_reached = 1;

        // if ((i == 1) && (T1_CLKSRC == 1))
        //     PRINTF("target %04x (%f) reached\r\n", timer->timer[i].target, timer->timer[i].counter);

        if (timer->timer[i].reset_target)
            timer->timer[i].counter = 0;

        if (timer->timer[i].irq_target)
            irq = 1;
    }

    if (max_reached)
    {
        timer->timer[i].counter = 0;
        timer->timer[i].max_reached = 1;

        if (timer->timer[i].irq_max)
            irq = 1;
    }

    if (!irq)
        return;

    if (!timer->timer[i].irq_toggle)
    {
        timer->timer[i].irq = 0;
    }
    else
    {
        timer->timer[i].irq ^= 1;
    }

    int32_t trigger = !timer->timer[i].irq;

    if (!timer->timer[i].irq_repeat)
    {
        if (trigger && !timer->timer[i].irq_fired)
        {
            timer->timer[i].irq_fired = 1;
        }
        else
        {
            return;
        }
    }

    timer->timer[i].irq = 1;

    if (trigger)
    {
        psx_ic_irq(timer->ic, 16 << i);
    }
}

static inline float timer_get_dotclock_div(psx_timer_t *timer)
{
    static const float dmode_dotclk_div_table[] = {
        10.0f, 8.0f, 5.0f, 4.0f};

    if (timer->gpu->display_mode & 0x40)
    {
        return 11.0f / 7.0f / 7.0f;
    }
    else
    {
        return 11.0f / 7.0f / dmode_dotclk_div_table[timer->gpu->display_mode & 0x3];
    }
}

static inline void timer_update_timer0(psx_timer_t *timer, int32_t cyc)
{
    if (T0_PAUSED)
        return;

    if (T0_CLKSRC & 1)
    {
        /* Dot clock: accumulate the fractional part in 16.16 */
        uint32_t frac = timer->timer[0].counter_frac +
                        (uint32_t)((float)cyc * timer_get_dotclock_div(timer) * (float)TIMER_FRAC_ONE);

        T0_COUNTER += frac >> 16;
        timer->timer[0].counter_frac = frac & 0xffffu;
    }
    else
    {
        T0_COUNTER += (uint32_t)cyc;
    }

    timer_handle_irq_inl(timer, 0);
}

static inline void timer_update_timer1(psx_timer_t *timer, int32_t cyc)
{
    if (T1_PAUSED)
        return;

    if (T1_CLKSRC & 1)
    {
        // Counter is incremented in our hblank callback
    }
    else
    {
        T1_COUNTER += (uint32_t)cyc;
    }

    timer_handle_irq_inl(timer, 1);
}

static inline void timer_update_timer2(psx_timer_t *timer, int32_t cyc)
{
    if (T2_PAUSED)
        return;

    if (T2_CLKSRC <= 1)
    {
        T2_COUNTER += (uint32_t)cyc;
    }
    else
    {
        /* System clock / 8 with the remainder kept in 16.16 */
        uint32_t frac = timer->timer[2].counter_frac + ((uint32_t)cyc << 13);

        T2_COUNTER += frac >> 16;
        timer->timer[2].counter_frac = frac & 0xffffu;
    }

    timer_handle_irq_inl(timer, 2);
}

/* Lower bound (in CPU cycles) on when the next counter event can happen. Every
   clock source ticks at most once per CPU cycle, so counting in counter units
   is always conservative. */
static void timer_recompute_deadline(psx_timer_t *timer)
{
    int32_t best = 0x40000000;

    for (int32_t i = 0; i < 3; i++)
    {
        if (timer->timer[i].paused)
            continue;

        /* Timer 1 driven by hblank does not advance with CPU cycles */
        if ((i == 1) && (timer->timer[i].clk_source & 1))
            continue;

        uint32_t limit = 65536u;

        if (timer->timer[i].irq_target || timer->timer[i].reset_target ||
            !timer->timer[i].target_reached)
        {
            uint32_t target = timer->timer[i].target;

            if ((target + 1u) < limit)
                limit = target + 1u;
        }

        int32_t remaining = (timer->timer[i].counter < limit)
                                ? (int32_t)(limit - timer->timer[i].counter)
                                : 1;

        if (remaining < best)
            best = remaining;
    }

    timer->deadline_cycles = best;
}

/* Applies the cycles accumulated since the last flush */
void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_timer_flush(psx_timer_t *timer)
{
    int32_t cyc = timer->pending_cycles;

    if (cyc <= 0)
        return;

    timer->pending_cycles = 0;

    timer_update_timer0(timer, cyc);
    timer_update_timer1(timer, cyc);
    timer_update_timer2(timer, cyc);

    timer_recompute_deadline(timer);
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_timer_update(psx_timer_t *timer, int32_t cyc)
{
    timer->pending_cycles += cyc;

    if (timer->pending_cycles < timer->deadline_cycles)
        return;

    psx_timer_flush(timer);
}

void psxe_gpu_hblank_event_cb(psx_gpu_t *gpu)
{
    psx_timer_t *timer = gpu->udata[1];

    timer->hblank = 1;

    if ((T1_CLKSRC & 1) && !T1_PAUSED)
    {
        ++T1_COUNTER;

        timer_handle_irq_inl(timer, 1);
    }

    if (!T0_SYNC_EN)
        return;

    switch (T0_SYNC_MODE)
    {
    case 0:
        T0_PAUSED = 1;
        break;
    case 1:
        T0_COUNTER = 0;
        break;
    case 2:
        T0_COUNTER = 0;
        T0_PAUSED = 0;
        break;
    case 3:
    {
        if (!T0_BLANK_ONCE)
        {
            T0_BLANK_ONCE = 1;
            T0_SYNC_EN = 0;
            T0_PAUSED = 0;
        }
    }
    break;
    }
}

void psxe_gpu_hblank_end_event_cb(psx_gpu_t *gpu)
{
    psx_timer_t *timer = gpu->udata[1];

    timer->hblank = 0;

    if (!T0_SYNC_EN)
        return;

    switch (T0_SYNC_MODE)
    {
    case 0:
        T0_PAUSED = 0;
        break;
    case 2:
        T0_PAUSED = 1;
        break;
    }
}

void psxe_gpu_vblank_timer_event_cb(psx_gpu_t *gpu)
{
    psx_timer_t *timer = gpu->udata[1];

    timer->vblank = 1;

    if (!T1_SYNC_EN)
        return;

    switch (T1_SYNC_MODE)
    {
    case 0:
        T1_PAUSED = 1;
        break;
    case 1:
        T1_COUNTER = 0;
        break;
    case 2:
        T1_COUNTER = 0;
        T1_PAUSED = 0;
        break;
    case 3:
    {
        if (!T1_BLANK_ONCE)
        {
            T1_BLANK_ONCE = 1;
            T1_SYNC_EN = 0;
            T1_PAUSED = 0;
        }
    }
    break;
    }
}

void psxe_gpu_vblank_end_event_cb(psx_gpu_t *gpu)
{
    psx_timer_t *timer = gpu->udata[1];

    timer->vblank = 0;

    if (!T1_SYNC_EN)
        return;

    switch (T1_SYNC_MODE)
    {
    case 0:
        T1_PAUSED = 0;
        break;
    case 2:
        T1_PAUSED = 1;
        break;
    }
}

void psx_timer_destroy(psx_timer_t *timer)
{
    // Mark instance as available again
    g_timer_instance_used = 0;
}
