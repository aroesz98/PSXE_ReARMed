/*
 * MCU-compatible screen driver for PSX emulator
 * Compatible with NXP MIMXRT1052 microcontroller
 */

#include "../gpu_switch.h"

#if PSXE_GPU_REMOTE
#include "../link/gpu_remote.h"
#include "../link/psxe_link.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "fsl_common.h"
#include "screen.h"

#include "input/sda.h"
#include "input/guncon.h"
#include "../psx.h"
#include "../dev/timer.h"

// FreeRTOS includes for MCU
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// Display includes for MCU
#include "display.h"
#include "display_support.h"
#include "fsl_pxp.h"
#include "fsl_cache.h"
#include "fsl_debug_console.h"
#include "../prof.h"
#include "../jit/jit.h"
#include "gamepad.h"

// Memory tracking - if available
#ifdef ENABLE_MEM_TRACKING
// Add memory tracking include if available
// #include "../mem_track.h"
#endif

// PXP hardware scaling definitions
#define APP_PXP PXP
#define APP_PXP_PS_FORMAT  kPXP_PsPixelFormatRGB565
#define APP_PXP_OUT_FORMAT kPXP_OutputPixelFormatRGB565

// Dynamic scaling selection: PXP for downscaling, software for upscaling

// GPIO mapping for buttons (example - adjust based on your hardware)
#define BUTTON_GPIO_PORT GPIO1
#define BUTTON_CROSS_PIN 20
#define BUTTON_SQUARE_PIN 21
#define BUTTON_TRIANGLE_PIN 22
#define BUTTON_CIRCLE_PIN 23
#define BUTTON_START_PIN 24
#define BUTTON_SELECT_PIN 25

// Simplified button function to avoid type issues
uint32_t screen_get_button_from_gpio(void)
{
    uint32_t button_mask = 0;

    // Read GPIO pins and map to PSX buttons
    // TODO: Implement actual GPIO reading
    // if (!GPIO_PinRead(BUTTON_GPIO_PORT, BUTTON_CROSS_PIN)) {
    //     button_mask |= PSXI_SW_SDA_CROSS;
    // }

    return button_mask;
}

// Static buffer for screen instance
static psxe_screen_t g_screen_instance;
static int32_t g_screen_instance_used = 0;

// PXP configuration for hardware scaling
static pxp_output_buffer_config_t g_pxp_output_config;
static pxp_ps_buffer_config_t g_pxp_ps_config;
static bool g_pxp_initialized = false;

int32_t screen_get_base_width(psxe_screen_t *screen)
{
    int32_t width = psx_get_dmode_width(screen->psx);

    switch (width)
    {
    case 256:
        return 256;
    case 320:
        return 320;
    case 368:
        return 384;
    }

    return 320;
}

// Initialize PXP hardware scaling engine
/*
    24 bpp display mode.

    In that mode the PSX display area holds packed 24 bit pixels - three bytes
    each, so a 320 pixel line takes 480 halfwords of VRAM. The PXP cannot read
    that: its "RGB888" process surface format is an unpacked 32 bit one. FF7 uses
    24 bpp for its full motion video, which came out as a green mush when the
    packed bytes were handed to the scaler as RGB565.

    So the display area is repacked into RGB565 first, into a staging buffer the
    scaler then reads. The buffer lives in SDRAM and is written through the
    D-cache; the clean before every PXP job covers it.
*/
#define SCREEN_STAGE_MAX_PIXELS (640 * 480)

static uint16_t __attribute__((section(".bss.$BOARD_SDRAM"), aligned(32)))
    g_rgb24_stage[SCREEN_STAGE_MAX_PIXELS];

/*
    Both repacks are SDRAM to SDRAM copies, and what they cost is memory latency,
    not arithmetic. Measured on this board:

      - a source line that is not in the cache stalls the core for about a
        hundred cycles, unless it was preloaded a few lines ahead - the core
        fetches a preloaded line in the background;
      - whole destination lines written one after the other need no line fill
        at all, but only as long as no load gets in between.

    So a row is converted into a buffer in DTCM first, with the source preloaded
    ahead of the loop, and then written out in one run of nothing but stores.
    That is about a third cheaper than converting straight across.
*/
static uint16_t __attribute__((section(".bss.$SRAM_DTC"), aligned(4))) g_repack_row[640];

#define SCREEN_PRELOAD_AHEAD 128 /* bytes, four cache lines */

static inline void screen_row_out(uint16_t *d, int32_t width)
{
    const uint16_t *r = g_repack_row;
    int32_t x = 0;

    for (; (x + 1) < width; x += 2)
    {
        uint32_t o;

        __builtin_memcpy(&o, &r[x], 4);
        __builtin_memcpy(&d[x], &o, 4);
    }

    if (x < width)
        d[x] = r[x];
}

/* Native PSX pixel (mask, blue, green, red) to the RGB565 the panel wants.
   `height` rows come out; every `row_step`-th source row goes in. */
static void screen_repack_bgr555(const uint16_t *src, int32_t width, int32_t height, int32_t row_step,
                                 uint16_t *dst)
{
    if (width > 640)
        width = 640;

    for (int32_t y = 0; y < height; y++)
    {
        const uint16_t *s = src + (uint32_t)(y * row_step) * (PSX_GPU_FB_STRIDE / 2u);
        uint16_t *const row = g_repack_row;

        int32_t x = 0;

        /* two pixels per 32 bit operation; the loads may be unaligned (the
           display window can start on an odd halfword), which the M7 handles */
        for (; (x + 1) < width; x += 2)
        {
            uint32_t p;

            if (!(x & 14))
                __builtin_prefetch((const uint8_t *)&s[x] + SCREEN_PRELOAD_AHEAD);

            __builtin_memcpy(&p, &s[x], 4);

            const uint32_t o = ((p & 0x001f001fu) << 11) | ((p & 0x03e003e0u) << 1) |
                               ((p >> 10) & 0x001f001fu);

            __builtin_memcpy(&row[x], &o, 4);
        }

        for (; x < width; x++)
        {
            const uint32_t p = s[x];

            row[x] = (uint16_t)(((p & 0x1fu) << 11) | (((p >> 5) & 0x1fu) << 6) | ((p >> 10) & 0x1fu));
        }

        screen_row_out(dst + (uint32_t)y * (uint32_t)width, width);
    }
}

/*
    Geometry test pattern, drawn instead of the emulated frame.

    Set PSXE_SCREEN_TEST_PATTERN to 1 to answer "does the panel show the whole
    source rectangle, and does it show it undistorted": a one pixel white frame
    around the edge, a differently coloured square in each corner, cross hairs
    through the middle and diagonals. A missing edge means the source rectangle
    is wrong, bent diagonals mean the row pitch is wrong, and a cut off frame
    means the output rectangle is off the panel.
*/
#ifndef PSXE_SCREEN_TEST_PATTERN
#define PSXE_SCREEN_TEST_PATTERN 0
#endif

#if PSXE_SCREEN_TEST_PATTERN
static void screen_test_pattern(int32_t width, int32_t height, uint16_t *dst)
{
    const uint16_t white = 0xffffu;
    const uint16_t red = 0xf800u;
    const uint16_t green = 0x07e0u;
    const uint16_t blue = 0x001fu;
    const uint16_t yellow = 0xffe0u;
    const uint16_t dim = 0x2104u;

    for (int32_t y = 0; y < height; y++)
    {
        uint16_t *d = dst + (uint32_t)y * (uint32_t)width;

        for (int32_t x = 0; x < width; x++)
        {
            uint16_t c = 0;

            /* diagonals, so any shear shows up as a kink */
            if ((((x + y) & 31) == 0) || (((x - y) & 31) == 0))
                c = dim;

            /* cross hairs */
            if ((x == (width / 2)) || (y == (height / 2)))
                c = white;

            /* one pixel frame */
            if ((x == 0) || (y == 0) || (x == (width - 1)) || (y == (height - 1)))
                c = white;

            /* corner markers, 12 pixels on a side */
            if ((x < 12) && (y < 12))
                c = red;
            else if ((x >= (width - 12)) && (y < 12))
                c = green;
            else if ((x < 12) && (y >= (height - 12)))
                c = blue;
            else if ((x >= (width - 12)) && (y >= (height - 12)))
                c = yellow;

            d[x] = c;
        }
    }
}
#endif

static void screen_repack_rgb24(const uint8_t *src, int32_t width, int32_t height, int32_t row_step,
                                uint16_t *dst)
{
    if (width > 640)
        width = 640;

    for (int32_t y = 0; y < height; y++)
    {
        const uint8_t *s = src + (uint32_t)(y * row_step) * PSX_GPU_FB_STRIDE;
        uint16_t *const d = g_repack_row;

        int32_t x = 0;

        /* one unaligned 32 bit load per pixel instead of three byte loads; the
           last pixel of a row is done bytewise so nothing is read past it */
        for (; x < (width - 1); x++, s += 3)
        {
            uint32_t p;

            if (!(x & 7))
                __builtin_prefetch(s + SCREEN_PRELOAD_AHEAD);

            __builtin_memcpy(&p, s, 4);

            d[x] = (uint16_t)(((p & 0xf8u) << 8) | ((p & 0xfc00u) >> 5) | ((p & 0xf80000u) >> 19));
        }

        for (; x < width; x++, s += 3)
            d[x] = (uint16_t)(((uint32_t)(s[0] & 0xf8u) << 8) | ((uint32_t)(s[1] & 0xfcu) << 3) |
                              ((uint32_t)s[2] >> 3));

        screen_row_out(dst + (uint32_t)y * (uint32_t)width, width);
    }
}

static void psxe_screen_init_pxp(void)
{
    if (g_pxp_initialized) {
        return; // Already initialized
    }

    // Initialize PXP peripheral
    PXP_Init(APP_PXP);

    // Configure Process Surface (input) - will be updated per frame
    g_pxp_ps_config.pixelFormat = APP_PXP_PS_FORMAT;
    g_pxp_ps_config.swapByte = false;
    g_pxp_ps_config.bufferAddr = 0U; // Will be set per frame
    g_pxp_ps_config.bufferAddrU = 0U;
    g_pxp_ps_config.bufferAddrV = 0U;
    g_pxp_ps_config.pitchBytes = 0U; // Will be set per frame

    // Set background color to black
#if defined(FSL_FEATURE_PXP_V3) && FSL_FEATURE_PXP_V3
    PXP_SetProcessSurfaceBackGroundColor(APP_PXP, 0U, 0U);
#else
    PXP_SetProcessSurfaceBackGroundColor(APP_PXP, 0U);
#endif

    // Disable Alpha Surface (AS)
    PXP_SetAlphaSurfacePosition(APP_PXP, 0xFFFFU, 0xFFFFU, 0U, 0U);

    // Configure Output buffer - will be updated per frame
    g_pxp_output_config.pixelFormat = APP_PXP_OUT_FORMAT;
    g_pxp_output_config.interlacedMode = kPXP_OutputProgressive;
    g_pxp_output_config.buffer0Addr = 0U; // Will be set per frame
    g_pxp_output_config.buffer1Addr = 0U;
    g_pxp_output_config.pitchBytes = LCD_WIDTH * 2; // RGB565 = 2 bytes per pixel
    g_pxp_output_config.width = LCD_WIDTH;  // Will be updated per frame
    g_pxp_output_config.height = LCD_HEIGHT; // Will be updated per frame

    // Disable CSC1 (Color Space Conversion)
    PXP_EnableCsc1(APP_PXP, false);

    g_pxp_initialized = true;
}

// static uint16_t __attribute__((section(".bss.$BOARD_SDRAM"))) g_temp_framebuffer[LCD_WIDTH * LCD_HEIGHT];

psxe_screen_t *psxe_screen_create(void)
{
    if (g_screen_instance_used)
    {
        return NULL; // Only one instance allowed
    }
    g_screen_instance_used = 1;

#ifdef ENABLE_MEM_TRACKING
    // Register static buffer size
    // add_static_buffer_size(sizeof(g_screen_instance), "screen_instance");
#endif

    return &g_screen_instance;
}

void psxe_screen_init(psxe_screen_t *screen, psx_t *psx)
{
    memset(screen, 0, sizeof(psxe_screen_t));

    if (screen->debug_mode)
    {
        screen->width = PSX_GPU_FB_WIDTH;
        screen->height = PSX_GPU_FB_HEIGHT;
    }
    else
    {
        screen->width = 320;
        screen->height = 240;
    }

    screen->scale = 1;
    screen->open = 1;
    screen->format = SCREEN_PIXELFORMAT_RGB565; // Use MCU format instead of SDL
    screen->psx = psx;
    screen->pad = psx_get_pad(psx);

    screen->texture_width = PSX_GPU_FB_WIDTH;
    screen->texture_height = PSX_GPU_FB_HEIGHT;

    // Initialize MCU display system
    DEMO_InitLcd();

    // Initialize PXP hardware scaling
    psxe_screen_init_pxp();

    // Initialize framebuffer pointers using display support functions
    screen->framebuffer = (void *)DEMO_GetCurrentFrameBuffer();
    screen->backbuffer = (void *)DEMO_GetCurrentFrameBuffer();
}

void psxe_screen_reload(psxe_screen_t *screen)
{
    // For MCU implementation, we don't need to recreate windows/renderers
    // Just update the display parameters

    if (screen->debug_mode)
    {
        screen->width = PSX_GPU_FB_WIDTH;
        screen->height = PSX_GPU_FB_HEIGHT;
    }
    else
    {
        if (screen->vertical_mode)
        {
            screen->width = 240;
            screen->height = screen_get_base_width(screen);
        }
        else
        {
            screen->width = screen_get_base_width(screen);
            screen->height = 240;
        }
    }

    screen->texture_width = PSX_GPU_FB_WIDTH;
    screen->texture_height = PSX_GPU_FB_HEIGHT;
    screen->open = 1;

    // Update framebuffer pointers
    screen->framebuffer = (void *)DEMO_GetCurrentFrameBuffer();
    screen->backbuffer = (void *)DEMO_GetCurrentFrameBuffer();
}

int32_t psxe_screen_is_open(psxe_screen_t *screen)
{
    return screen->open;
}

void psxe_screen_toggle_debug_mode(psxe_screen_t *screen)
{
    screen->debug_mode = !screen->debug_mode;

    psxe_screen_set_scale(screen, screen->saved_scale);

    screen->texture_width = PSX_GPU_FB_WIDTH;
    screen->texture_height = PSX_GPU_FB_HEIGHT;

    psxe_gpu_dmode_event_cb(screen->psx->gpu);
}

volatile uint32_t log = 0;

static void psxe_screen_update_impl(psxe_screen_t *screen, uint32_t dirty_y0, uint32_t dirty_y1);
static void psxe_screen_poll_pxp(void);
#if PSXE_GPU_REMOTE
static void psxe_screen_send_frame(psxe_screen_t *screen, uint32_t dirty_y0, uint32_t dirty_y1);
#endif

static uint32_t g_presented_frames = 0;
static int32_t s_pxp_busy = 0;

#if PSXE_AUTOTEST
static uint32_t g_diag_stage_rows, g_diag_stage_full, g_diag_stage_checks, g_diag_stage_bad;
static uint32_t g_diag_pxp_start, g_diag_pxp_last, g_diag_pxp_jobs, g_diag_pxp_stillbusy;
#endif

/*
    Real time pacing.

    The emulator used to be slower than a PlayStation everywhere, so it simply
    ran flat out. It no longer is: menus, dialogue and the BIOS now run at 1.3 to
    2 times real speed, which is as wrong as too slow. Every emulated vertical
    blank therefore has a time at which it is due, a frame's worth of real time
    after the previous one, and one that comes early waits for it.

    Set PSXE_FRAME_LIMIT to 0 to run unthrottled, e.g. to measure a change.
*/
#ifndef PSXE_FRAME_LIMIT
#define PSXE_FRAME_LIMIT 1
#endif

/* Free running core cycle counter (wraps every 7 s at 600 MHz; only ever used
   for differences well below that). The profiler uses it as well, but it may
   be compiled out, so it is switched on here. */
static inline uint32_t psxe_screen_cycles(void)
{
    static int32_t enabled = 0;

    if (!enabled)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

        enabled = 1;
    }

    return DWT->CYCCNT;
}

/* 263 lines of 3413 GPU cycles at 53.693175 MHz: what the GPU model counts out */
#define PSXE_FRAME_MICROSECONDS 16718u

/*
    How the emulation is doing against real time, 0 (keeping up) to 8 (behind on
    every recent frame). A single late frame means nothing - a game that renders
    every other vblank is late after each long frame and early after each short
    one - so this goes up by one for a late vblank and down by one for one that
    was on time.
*/
static int32_t s_behind = 0;
static uint32_t s_vblank_no = 0; /* emulated vblanks so far */

#define PSXE_BEHIND (s_behind >= 4)

static void psxe_screen_pace(void)
{
    s_vblank_no++;

    static uint32_t due = 0; /* when this vblank should happen, in core cycles */
    static int32_t primed = 0;

    const uint32_t period = (uint32_t)(((uint64_t)SystemCoreClock * PSXE_FRAME_MICROSECONDS) / 1000000u);
    const uint32_t tick = SystemCoreClock / configTICK_RATE_HZ;

    const uint32_t now = psxe_screen_cycles();

    if (!primed)
    {
        primed = 1;
        due = now;

        return;
    }

    due += period;

    /* The counter wraps, so "how late" is a signed difference; both sides stay
       within a few frames of each other, far below half its range. */
    const int32_t late = (int32_t)(now - due);

    if (late >= 0)
    {
        if (s_behind < 8)
            s_behind++;

        /*
            Behind schedule: nothing to wait for. The schedule is kept, though,
            so that the shorter frames that follow make the time up again. A game
            that renders every other vblank alternates a long frame with a short
            one; waiting after every short one while writing the long ones off
            cost such scenes a fifth of their speed (88% became 70%), and the
            bursty video decoder much the same.

            Only so far: more than a few frames behind means the emulation simply
            is slower than real time here, and a schedule kept through that would
            be a debt the next light scene pays back by running fast.
        */
        if ((uint32_t)late > (4u * period))
            due = now;

        return;
    }

    if (s_behind > 0)
        s_behind--;

#if PSXE_FRAME_LIMIT
    /* ahead: sleep through the whole ticks, spin through the rest */
    for (;;)
    {
        const int32_t left = (int32_t)(due - psxe_screen_cycles());

        if (left <= 0)
            break;

        if ((uint32_t)left > (2u * tick))
            vTaskDelay(1);
    }
#else
    /* running unthrottled (a measurement build): the schedule is only kept to
       know when the emulation falls behind it */
    (void)tick;

    if ((uint32_t)(-late) > period)
        due = now;
#endif
}

#if PSXE_AUTOTEST
/* What is on screen, as 64 x 24 characters on the console: an unattended run
   has nobody looking at the panel. 15 bpp pictures only. */
static void psxe_screen_thumbnail(psxe_screen_t *screen)
{
    static const char ramp[] = " .:-=+*#%@";

    psx_gpu_t *const gpu = screen->psx->gpu;

    const int32_t w = (int32_t)psx_get_display_width(screen->psx);
    const int32_t h = 240;

    if (psx_get_display_format(screen->psx))
    {
        PRINTF("THUMB 24bpp picture, not drawn" "\r\n");
        return;
    }

    PRINTF("THUMB %dx%d at (%u,%u)" "\r\n", (int)w, (int)h, (unsigned)gpu->disp_x, (unsigned)gpu->disp_y);

    for (int32_t ty = 0; ty < 24; ty++)
    {
        char line[66];

        for (int32_t tx = 0; tx < 64; tx++)
        {
            const uint32_t x = (gpu->disp_x + (uint32_t)((tx * w) / 64)) & 0x3ffu;
            const uint32_t y = (gpu->disp_y + (uint32_t)((ty * h) / 24)) & 0x1ffu;
            const uint32_t p = gpu->vram[PSX_VRAM_AT(x, y)];

            const uint32_t luma = ((p & 0x1fu) * 2u + ((p >> 5) & 0x1fu) * 5u + ((p >> 10) & 0x1fu)) / 8u;

            line[tx] = ramp[(luma * 9u) / 31u];
        }

        line[64] = 0;

        PRINTF("|%s|" "\r\n", line);
    }
}
#endif

void psxe_screen_update(psxe_screen_t *screen)
{
    psx_gpu_t *const gpu = screen->psx->gpu;

    /* Speed report once a second. Runs on the emulated vblank (not per
       instruction), so it costs nothing measurable.
       emu = emulated CPU cycles per second, 33869 kcyc/s is a real PS1. */
    {
        static uint32_t last_tick = 0;
        static uint32_t last_cycles = 0;
        static uint32_t vblanks = 0;

        uint32_t now = xTaskGetTickCount();

        vblanks++;

        if ((now - last_tick) >= configTICK_RATE_HZ)
        {
            uint32_t cycles = screen->psx->cpu->total_cycles;
            uint32_t kcyc = (cycles - last_cycles) / 1000u;

            const psx_jit_stats_t *jit = psx_jit_get_stats();

#if PSXE_GPU_REMOTE
            {
                /* while the GPU board draws: the frames it shows, since the last report */
                static uint32_t last_presented = 0;
                const uint32_t presented = gpu_remote_presented();

                (void)presented;
                last_presented = presented;

                gpu_remote_report();
            }
#endif

#if PSX_JIT_HIST
            {
                /* the five opcode classes the fallback spends most time on */
                const uint32_t *hist = psx_jit_get_hist();

                uint32_t top[5] = {0, 0, 0, 0, 0};

                for (uint32_t i = 0; i < PSX_JIT_HIST_SIZE; i++)
                {
                    for (uint32_t k = 0; k < 5; k++)
                    {
                        if (hist[i] > hist[top[k]])
                        {
                            for (uint32_t m = 4; m > k; m--)
                                top[m] = top[m - 1];

                            top[k] = i;
                            break;
                        }
                    }
                }

                PRINTF("jit fallback: ");

                for (uint32_t k = 0; k < 5; k++)
                {
                    if (!hist[top[k]])
                        continue;

                    if (top[k] < 64u)
                        PRINTF("op%02x=%u ", (unsigned)top[k], (unsigned)hist[top[k]]);
                    else
                        PRINTF("sp%02x=%u ", (unsigned)(top[k] - 64u), (unsigned)hist[top[k]]);
                }

                PRINTF("\r\n");

                psx_jit_clear_hist();
            }
#endif

#if PSX_PROFILE || PSXE_AUTOTEST
            PRINTF("emu: %u kcyc/s (%u%% of PS1) | vbl/s: %u | fps: %u | jit blk=%u cmp=%u flush=%u inv=%u code=%uB hot=%uB/%u tier=%u int=%u disp=%u cold=%u\r\n",
                   kcyc, (unsigned)((kcyc * 100u) / 33869u), vblanks, g_presented_frames,
                   (unsigned)jit->blocks, (unsigned)jit->compiles, (unsigned)jit->flushes,
                   (unsigned)jit->invalidations, (unsigned)jit->code_used,
                   (unsigned)jit->hot_used, (unsigned)jit->hot_blocks, (unsigned)jit->retiers,
                   (unsigned)jit->interp_steps, (unsigned)jit->dispatches, (unsigned)jit->cold_dispatches);
#else
            /* a character is 87 microseconds of blocking UART: the long line is 1.7%
               of the machine, so the build that is meant to be played prints a short one */
            PRINTF("emu: %u%% of PS1 | vbl/s: %u | fps: %u | jit blk=%u flush=%u\r\n",
                   (unsigned)((kcyc * 100u) / 33869u), vblanks, g_presented_frames,
                   (unsigned)jit->blocks, (unsigned)jit->flushes);
#endif

/* Display mode and pad link counters: for chasing display problems, so they
   come with the profiler rather than costing UART time in a build that is
   meant to be played. */
#if PSX_PROFILE
            {
                /* Frames from the ESP32 pad bridge, so a wiring or baud rate
                   problem is visible without a debugger: frames climbing means
                   the link is alive, errors climbing means the line is noisy. */
                uint32_t pad_frames = 0;
                uint32_t pad_errors = 0;

                psxe_gamepad_get_stats(&pad_frames, &pad_errors);

                PRINTF("disp: %dx%d %s mode=%03x yrange=%u..%u start=(%u,%u) | pad: %u/%u\r\n",
                       (int)psx_get_display_width(screen->psx),
                       (int)psx_get_display_height(screen->psx),
                       psx_get_display_format(screen->psx) ? "24bpp" : "15bpp",
                       (unsigned)screen->psx->gpu->display_mode,
                       (unsigned)screen->psx->gpu->disp_y1, (unsigned)screen->psx->gpu->disp_y2,
                       (unsigned)screen->psx->gpu->disp_x, (unsigned)screen->psx->gpu->disp_y,
                       (unsigned)pad_frames, (unsigned)pad_errors);
            }
#endif

#if PSX_JIT_HIST
            PRINTF("jit dispatch-interp=%u\r\n", (unsigned)jit->dispatch_steps);
#endif

#if PSXE_AUTOTEST
            {
                /* how fast is SDRAM right now: 64 KB of VRAM, read cold */
                const uint16_t *v = (const uint16_t *)screen->psx->gpu->vram + PSX_VRAM_AT(0u, 300u);
                uint32_t sum = 0;
                const uint32_t t0 = DWT->CYCCNT;

                for (uint32_t i = 0; i < 32768u; i += 16u)
                    sum += v[i];

                const uint32_t t1 = DWT->CYCCNT;

                PRINTF("STAGE rows=%u of %u checks=%u badrows=%u" "\r\n",
                       (unsigned)g_diag_stage_rows, (unsigned)g_diag_stage_full, (unsigned)g_diag_stage_checks,
                       (unsigned)g_diag_stage_bad);

                g_diag_stage_rows = 0;
                g_diag_stage_full = 0;

                PRINTF("DIAG stackfree=%u ccr=%08x sdramcr0=%08x sdram64k=%u cyc (sum %u) | pxp last=%u cyc jobs=%u stillbusy=%u" "\r\n",
                       (unsigned)(uxTaskGetStackHighWaterMark(NULL) * 4u), (unsigned)SCB->CCR, (unsigned)SEMC->SDRAMCR0, (unsigned)(t1 - t0), (unsigned)sum,
                       (unsigned)g_diag_pxp_last, (unsigned)g_diag_pxp_jobs, (unsigned)g_diag_pxp_stillbusy);

                PRINTF("DIAG-HW lcdif ctrl=%08x ctrl1=%08x stat=%08x cur=%08x next=%08x | pllv=%08x cscdr2=%08x cbcmr=%08x | pxp ctrl=%08x stat=%08x | semc sts0=%08x intr=%08x\r\n",
                       (unsigned)LCDIF->CTRL, (unsigned)LCDIF->CTRL1, (unsigned)LCDIF->STAT,
                       (unsigned)LCDIF->CUR_BUF, (unsigned)LCDIF->NEXT_BUF,
                       (unsigned)CCM_ANALOG->PLL_VIDEO, (unsigned)CCM->CSCDR2, (unsigned)CCM->CBCMR,
                       (unsigned)PXP->CTRL, (unsigned)PXP->STAT, (unsigned)SEMC->STS0, (unsigned)SEMC->INTR);

#if PSXE_AUTOTEST_REBOOT_S
                {
                    /* boot soak: see what state each boot ends up in, then go again */
                    static uint32_t up = 0;

                    if (++up >= PSXE_AUTOTEST_REBOOT_S)
                        NVIC_SystemReset();
                }
#endif

                g_diag_pxp_jobs = 0;
                g_diag_pxp_stillbusy = 0;
            }

            {
                static uint32_t seconds = 0;

                if ((++seconds % 15u) == 0u)
                    psxe_screen_thumbnail(screen);
            }
#endif

            last_tick = now;
            last_cycles = cycles;
            vblanks = 0;
            g_presented_frames = 0;
        }
    }

    psxe_screen_pace();


    /* The frame the scaler was given at the last vblank goes to the panel now.
       This must not wait for the next changed picture: there may not be one for
       a long time - a dialogue box waiting for a button - and the frame in the
       scaler is the box. */
    psxe_screen_poll_pxp();

    /* Nothing visible changed since the last presented frame: the LCD already
       shows this picture, so re-scaling it would be pure overhead. */
    if (!gpu->vram_dirty && !screen->debug_mode)
        return;

    /* A game that draws one field per frame (480 line interlaced, gpu.c has it
       under gpu_update_field) has just put one of the two on screen and draws
       into the other next. The picture made here is every other row from
       disp_y on (psxe_screen_update_impl): when those are the rows about to be
       drawn into, they still hold what was presented two blanks ago, and the
       picture waits for the next blank. */
    if (!screen->debug_mode && (gpu->skip_rows >= 0) && ((uint32_t)gpu->skip_rows != (gpu->disp_y & 1u)))
        return;

    /* The panel takes one frame per refresh, so frames started closer together
       than that only replace one another unseen: an emulation running above
       real time was paying a full repack and scale for pictures nobody saw. The
       picture stays marked as changed, so the next vblank tries again with
       whatever is newest then. (15 ms rather than the panel's 16.7, so that a
       game running at exactly the panel's rate is never made to skip.) */
    {
        static uint32_t last_start = 0;
        static uint32_t last_vblank = 0;

        const uint32_t now_cyc = psxe_screen_cycles();

        if (s_pxp_busy || ((now_cyc - last_start) < ((SystemCoreClock / 1000u) * 15u)))
            return;

        /* A present is over a million cycles, and while the emulation cannot
           keep up with real time those are better spent on the game itself:
           then never on two vblanks in a row. Nothing is lost that way - the
           picture stays marked as changed, so it is shown one vblank later, and
           what needs a frame every vblank is not reaching full speed anyway. */
        if (PSXE_BEHIND && ((s_vblank_no - last_vblank) < 2u))
            return;

        last_start = now_cyc;
        last_vblank = s_vblank_no;
    }

    gpu->vram_dirty = 0;

    /* the rows of VRAM that changed on screen; from here on the GPU collects anew */
    const uint32_t dirty_y0 = gpu->dirty_y0;
    const uint32_t dirty_y1 = gpu->dirty_y1;

    gpu->dirty_y0 = 0xffffu;
    gpu->dirty_y1 = 0;

    PROF_T0(t_blit);
    PROF_INC(frames);
#if PSXE_GPU_REMOTE
    if (g_gpu_remote && g_gpu_stream)
    {
        /* the GPU board has the whole picture: nothing is drawn or shown here */
        g_presented_frames++;
    }
    else if (g_gpu_remote)
    {
        psxe_screen_send_frame(screen, dirty_y0, dirty_y1);
    }
    else
#endif
        psxe_screen_update_impl(screen, dirty_y0, dirty_y1);
    PROF_ADD(blit, t_blit);
}

/* Hands the frame over to the panel if the scaler is done with it */
static void psxe_screen_poll_pxp(void)
{
#if PSXE_AUTOTEST
    if (s_pxp_busy && !(kPXP_CompleteFlag & PXP_GetStatusFlags(APP_PXP)))
        g_diag_pxp_stillbusy++;
#endif

    if (!s_pxp_busy || !(kPXP_CompleteFlag & PXP_GetStatusFlags(APP_PXP)))
        return;

#if PSXE_AUTOTEST
    g_diag_pxp_last = DWT->CYCCNT - g_diag_pxp_start;
    g_diag_pxp_jobs++;
#endif

    PXP_ClearStatusFlags(APP_PXP, kPXP_CompleteFlag);

    s_pxp_busy = 0;

    DEMO_SwapBuffers();
}

/* Present the frame PXP was working on (started during the previous update) */
static void psxe_screen_finish_pxp(void)
{
    if (!s_pxp_busy)
        return;

    PROF_T0(t_wait);

    while (!(kPXP_CompleteFlag & PXP_GetStatusFlags(APP_PXP)))
    {
        /* hardware scaling still in progress */
    }

    PROF_ADD(bwait, t_wait);

    PXP_ClearStatusFlags(APP_PXP, kPXP_CompleteFlag);

    s_pxp_busy = 0;

    DEMO_SwapBuffers();
}

#if PSXE_GPU_REMOTE
/*
    The hybrid: this board rasterized the picture, the GPU board shows it. What
    goes over the link is the display window as it stands in VRAM, in the PSX's
    own pixel format - only the rows drawn into since the last picture, unless
    the window itself moved, which is what the local repack does too.

    The GPU board's panel is 800x480, so a 480 line picture goes over whole;
    the local path halves it because its own panel has 272 lines.
*/
static void psxe_screen_send_frame(psxe_screen_t *screen, uint32_t dirty_y0, uint32_t dirty_y1)
{
    psx_gpu_t *const gpu = screen->psx->gpu;

    int32_t w = psx_get_display_width(screen->psx);
    int32_t h = psx_get_display_height(screen->psx);

    if ((w <= 0) || (w > 640))
        w = 320;

    if ((h <= 0) || (h > 480))
        h = 240;

    if ((w == 320) && (h > 240))
        h = 240;

    if (((int32_t)gpu->disp_y + h) > PSX_GPU_FB_HEIGHT)
        h = PSX_GPU_FB_HEIGHT - (int32_t)gpu->disp_y;

    if (h <= 0)
        h = 1;

    const int32_t is_24 = psx_get_display_format(screen->psx) != 0;
    const int blank = (gpu->gpustat & 0x800000u) != 0;

    /*
        A game that draws one field of an interlaced picture per frame (gpu.c,
        gpu_update_field; Tekken 3 at 368x480) has just finished the field that
        starts at disp_y. Sending both fields would put a fresh one next to one
        a frame old - which combs, and costs twice the wire - so only every
        other row goes, and the GPU board scales the 240 rows to its panel.
    */
    const int32_t row_step = ((gpu->skip_rows >= 0) && (h > 240)) ? 2 : 1;

    h /= row_step;

    uint32_t flags = (is_24 ? PSXE_FRAME_24BPP : 0u) | (blank ? PSXE_FRAME_BLANK : 0u);

    /* the rows that changed, in the window's coordinates; anything else about
       the window having changed makes it a new picture */
    static uint32_t sent_x = ~0u, sent_y = ~0u;
    static int32_t sent_w = -1, sent_h = -1, sent_24 = -1, sent_step = -1;

    uint32_t row0 = 0;
    uint32_t row1 = (uint32_t)h;

    if ((sent_x == gpu->disp_x) && (sent_y == gpu->disp_y) && (sent_w == w) && (sent_h == h) &&
        (sent_24 == is_24) && (sent_step == row_step) && (dirty_y0 < dirty_y1))
    {
        const int32_t first = ((int32_t)dirty_y0 - (int32_t)gpu->disp_y) / row_step;
        const int32_t last = ((int32_t)dirty_y1 - (int32_t)gpu->disp_y + row_step - 1) / row_step;

        if (first > 0)
            row0 = (first < h) ? (uint32_t)first : (uint32_t)h;

        if (last < h)
            row1 = (last > (int32_t)row0) ? (uint32_t)last : row0;
    }
    else
    {
        flags |= PSXE_FRAME_WHOLE;
    }

    const void *src = psx_get_display_buffer(screen->psx);

    if (blank || !src)
    {
        row0 = 0;
        row1 = 0;
    }

    if (gpu_remote_frame(src, (uint32_t)row_step * PSX_GPU_FB_STRIDE, (uint32_t)w, (uint32_t)h, row0,
                         row1, flags, gpu->display_mode) != 0)
        return; /* the link went: the next picture is a whole one, and it may well be local */

    sent_x = gpu->disp_x;
    sent_y = gpu->disp_y;
    sent_w = w;
    sent_h = h;
    sent_24 = is_24;
    sent_step = row_step;

    g_presented_frames++;
}
#endif

static void psxe_screen_update_impl(psxe_screen_t *screen, uint32_t dirty_y0, uint32_t dirty_y1)
{
    static int32_t last_scaled_w = -1, last_scaled_h = -1;
    static int32_t clear_pending = 3; /* one per frame buffer */

    /* finish and present the previously started scaling job */
    psxe_screen_finish_pxp();

    void *display_buf = screen->debug_mode ? psx_get_vram(screen->psx) : psx_get_display_buffer(screen->psx);

    uint16_t *src = (uint16_t *)display_buf;

    if (!src)
        return;

    /* Get actual PSX output dimensions dynamically with sanity checks */
    int32_t src_width, src_height;

    if (screen->debug_mode)
    {
        src_width = PSX_GPU_FB_WIDTH;   /* 1024 */
        src_height = PSX_GPU_FB_HEIGHT; /* 512 */
    }
    else
    {
        src_width = psx_get_display_width(screen->psx);
        src_height = psx_get_display_height(screen->psx);

        if (src_width <= 0 || src_width > 640)
            src_width = 320;

        if (src_height <= 0 || src_height > 480)
            src_height = 240;

        if (src_width == 320 && src_height > 240)
            src_height = 240;

        if (src_width == 640 && src_height > 480)
            src_height = 480;

        /* The window has to stay inside VRAM. This used to fall back to the top
           of VRAM instead, which showed the wrong buffer on every frame the game
           displayed its second one (disp_y = 240 with FF7's double buffering)
           and turned into stripes whenever the two buffers differed. */
        if ((int32_t)screen->psx->gpu->disp_y + src_height > PSX_GPU_FB_HEIGHT)
            src_height = PSX_GPU_FB_HEIGHT - (int32_t)screen->psx->gpu->disp_y;

        if (src_height <= 0)
            src_height = 1;
    }

    const int32_t dst_width = LCD_WIDTH;   /* 480 */
    const int32_t dst_height = LCD_HEIGHT; /* 272 */

    /*
        Fit a 4:3 rectangle into the panel, not the source pixel count.

        Every standard PSX video mode covers the same screen area: 256, 320, 368,
        512 and 640 pixel wide modes all span the full width, and 240 or 480
        lines both span the full height, so the pixels are not square. Scaling by
        the pixel counts squashed the 640x240 modes (FF7's menus) into 480x180
        with black bars above and below.

        Set PSXE_SCREEN_FILL_PANEL to 1 to stretch to the whole 480x272 panel
        instead: no bars, but a 4:3 picture comes out a third too wide.
    */
#ifndef PSXE_SCREEN_FILL_PANEL
#define PSXE_SCREEN_FILL_PANEL 0
#endif

#if PSXE_SCREEN_FILL_PANEL
    int32_t scaled_width = dst_width;
    int32_t scaled_height = dst_height;
#else
    int32_t scaled_height = dst_height;
    int32_t scaled_width = (dst_height * 4) / 3;

    if (scaled_width > dst_width)
    {
        scaled_width = dst_width;
        scaled_height = (dst_width * 3) / 4;
    }
#endif

    const int32_t x_offset = (dst_width - scaled_width) / 2;
    const int32_t y_offset = (dst_height - scaled_height) / 2;

    uint16_t *dst = (uint16_t *)DEMO_GetCurrentFrameBuffer();

    if ((scaled_width != last_scaled_w) || (scaled_height != last_scaled_h))
    {
        last_scaled_w = scaled_width;
        last_scaled_h = scaled_height;

        /* All three buffers have to lose the old letterbox bars */
        clear_pending = 3;

        PRINTF("PXP scaling: %dx%d -> %dx%d, offset=(%d,%d), vram start=(%u,%u), mode=%03x\r\n",
               src_width, src_height, scaled_width, scaled_height, x_offset, y_offset,
               (unsigned)screen->psx->gpu->disp_x, (unsigned)screen->psx->gpu->disp_y,
               (unsigned)screen->psx->gpu->display_mode);
    }

    if (clear_pending > 0)
    {
        clear_pending--;

        memset(dst, 0, dst_width * dst_height * sizeof(uint16_t));
    }

    /*
        VRAM is in the native PSX pixel format, which the scaler cannot read, so
        the visible area is repacked into RGB565 first - from 15 bpp pixels
        normally, from packed 24 bpp ones during full motion video.
    */
    const uint16_t *ps_buffer = src;
    uint32_t ps_pitch = PSX_GPU_FB_STRIDE;

/* Set to 0 to feed 15 bpp frames to the scaler straight out of VRAM, as before
   the native format change. 24 bpp always needs the repack. */
#ifndef PSXE_SCREEN_USE_STAGING
#define PSXE_SCREEN_USE_STAGING 1
#endif

    const int32_t is_24bpp = psx_get_display_format(screen->psx) && !screen->debug_mode;

    if (((src_width * src_height) <= SCREEN_STAGE_MAX_PIXELS) &&
        (is_24bpp || PSXE_SCREEN_USE_STAGING))
    {
#if PSXE_SCREEN_TEST_PATTERN
        screen_test_pattern(src_width, src_height, g_rgb24_stage);
#else
        /*
            A 480 line picture is scaled down to the panel's 272, so every other
            source row is enough to fill it - and the repack, which is bound by
            reading the picture out of SDRAM, costs half as much. (An interlaced
            display shows one such field per refresh anyway. A game that draws
            only one field per frame is presented on the blanks that put these
            rows on screen, see psxe_screen_update.)
        */
        const int32_t row_step = (src_height > 272) ? 2 : 1;

        src_height /= row_step;

        /*
            The repacked picture stays in g_rgb24_stage between frames, so when
            the window into VRAM is the one it was made from, only the rows that
            were drawn into since have to be read again: FF7 draws its battle
            menus over the visible buffer twice between two flips, and that is
            a quarter of the picture, not all of it. Anything else about the
            window having changed - position, size, depth - is a new picture.
        */
        static uint32_t staged_x = ~0u, staged_y = ~0u;
        static int32_t staged_w = -1, staged_h = -1, staged_24 = -1, staged_step = -1;

        int32_t row0 = 0;
        int32_t row1 = src_height;

        const uint32_t win_x = screen->psx->gpu->disp_x;
        const uint32_t win_y = screen->psx->gpu->disp_y;

        if (!screen->debug_mode && (staged_x == win_x) && (staged_y == win_y) && (staged_w == src_width) &&
            (staged_h == src_height) && (staged_24 == is_24bpp) && (staged_step == row_step) &&
            (dirty_y0 < dirty_y1))
        {
            const int32_t first = ((int32_t)dirty_y0 - (int32_t)win_y) / row_step;
            const int32_t last = ((int32_t)dirty_y1 - (int32_t)win_y + row_step - 1) / row_step;

            if (dirty_y0 > win_y)
                row0 = (first < src_height) ? first : src_height;

            if (last < src_height)
                row1 = (last > row0) ? last : row0;
        }

        staged_x = screen->debug_mode ? ~0u : win_x;
        staged_y = win_y;
        staged_w = src_width;
        staged_h = src_height;
        staged_24 = is_24bpp;
        staged_step = row_step;

#if PSXE_AUTOTEST
        g_diag_stage_rows += (uint32_t)(row1 - row0);
        g_diag_stage_full += (uint32_t)src_height;
#endif

        if (row1 > row0)
        {
            uint16_t *const out = g_rgb24_stage + (uint32_t)row0 * (uint32_t)src_width;

            if (is_24bpp)
                screen_repack_rgb24((const uint8_t *)src + (uint32_t)(row0 * row_step) * PSX_GPU_FB_STRIDE,
                                    src_width, row1 - row0, row_step, out);
            else
                screen_repack_bgr555(src + (uint32_t)(row0 * row_step) * (PSX_GPU_FB_STRIDE / 2u), src_width,
                                     row1 - row0, row_step, out);
        }
#if PSXE_AUTOTEST
        /* Self check of the partial repack: now and then, is the staged picture
           what a full repack would have made? Rows that are not count. */
        {
            static uint32_t presents = 0;

            if (!is_24bpp && ((++presents & 31u) == 0u))
            {
                for (int32_t y = 0; y < src_height; y++)
                {
                    const uint16_t *const s = src + (uint32_t)(y * row_step) * (PSX_GPU_FB_STRIDE / 2u);
                    const uint16_t *const d = g_rgb24_stage + (uint32_t)y * (uint32_t)src_width;

                    for (int32_t x = 0; x < src_width; x++)
                    {
                        const uint32_t v = s[x];
                        const uint16_t want = (uint16_t)(((v & 0x1fu) << 11) | (((v >> 5) & 0x1fu) << 6) | ((v >> 10) & 0x1fu));

                        if (d[x] != want)
                        {
                            if (g_diag_stage_bad++ < 6u)
                                PRINTF("STAGE-BAD row=%d x=%d repacked=[%d,%d) dirty=[%u,%u) win=(%u,%u) %dx%d step=%d" "\r\n",
                                       (int)y, (int)x, (int)row0, (int)row1, (unsigned)dirty_y0, (unsigned)dirty_y1,
                                       (unsigned)win_x, (unsigned)win_y, (int)src_width, (int)src_height, (int)row_step);

                            break;
                        }
                    }
                }

                g_diag_stage_checks++;
            }
        }
#endif
#endif

        ps_buffer = g_rgb24_stage;
        ps_pitch = (uint32_t)src_width * 2u;
    }

    /* The rasterizer writes VRAM through the D-cache, PXP reads it as a bus
       master - a full clean is cheaper than cleaning the display window. */
    SCB_CleanDCache();

    /* Process surface = PSX display area inside VRAM */
    g_pxp_ps_config.bufferAddr = (uint32_t)ps_buffer;
    g_pxp_ps_config.pitchBytes = ps_pitch;
    PXP_SetProcessSurfaceBufferConfig(APP_PXP, &g_pxp_ps_config);

    /* Hardware scaler handles both down- and upscaling */
    PXP_SetProcessSurfaceScaler(APP_PXP, src_width, src_height, scaled_width, scaled_height);
    PXP_SetProcessSurfacePosition(APP_PXP, x_offset, y_offset,
                                  x_offset + scaled_width - 1, y_offset + scaled_height - 1);

    g_pxp_output_config.buffer0Addr = (uint32_t)dst;
    g_pxp_output_config.width = dst_width;
    g_pxp_output_config.height = dst_height;
    g_pxp_output_config.pitchBytes = dst_width * 2;
    PXP_SetOutputBufferConfig(APP_PXP, &g_pxp_output_config);

    PXP_Start(APP_PXP);

#if PSXE_AUTOTEST
    g_diag_pxp_start = DWT->CYCCNT;
#endif

    /* Do not block here: the emulator keeps running while PXP scales the
       frame, the result is presented at the beginning of the next frame. */
    s_pxp_busy = 1;

    g_presented_frames++;
}

void psxe_screen_set_scale(psxe_screen_t *screen, uint32_t scale)
{
    if (screen->debug_mode)
    {
        screen->scale = 1;
    }
    else
    {
        screen->scale = scale;
        screen->saved_scale = scale;
    }
}

void psxe_screen_destroy(psxe_screen_t *screen)
{
    // No need to destroy textures/renderers/windows on MCU
    // Just clean up state and mark instance as available again
    screen->open = 0;
    g_screen_instance_used = 0;
}

void psxe_gpu_dmode_event_cb(psx_gpu_t *gpu)
{
    psxe_screen_t *screen = gpu->udata[0];

    // Set display format based on PSX GPU state
    screen->format = psx_get_display_format(screen->psx) ? SCREEN_PIXELFORMAT_RGB24 : SCREEN_PIXELFORMAT_RGB565;

    if (screen->debug_mode)
    {
        screen->width = PSX_GPU_FB_WIDTH;
        screen->height = PSX_GPU_FB_HEIGHT;
        screen->texture_width = PSX_GPU_FB_WIDTH;
        screen->texture_height = PSX_GPU_FB_HEIGHT;
    }
    else
    {
        if (screen->vertical_mode)
        {
            screen->width = 240 * screen->scale;
            screen->height = screen_get_base_width(screen) * screen->scale;
            screen->image_width = screen->height;
            screen->image_height = screen->width;

            int32_t off = (screen->image_width - screen->image_height) / 2;
            screen->image_xoff = -off;
            screen->image_yoff = off;
        }
        else
        {
            screen->width = screen_get_base_width(screen) * screen->scale;
            screen->height = 240 * screen->scale;
            screen->image_width = screen->width;
            screen->image_height = screen->height;
            screen->image_xoff = 0;
            screen->image_yoff = 0;
        }

        screen->texture_width = psx_get_display_width(screen->psx);
        screen->texture_height = psx_get_display_height(screen->psx);
    }
}

void psxe_gpu_vblank_event_cb(psx_gpu_t *gpu)
{
    psxe_screen_t *screen = gpu->udata[0];

    psxe_screen_update(screen);

    psxe_gpu_vblank_timer_event_cb(gpu);
}
