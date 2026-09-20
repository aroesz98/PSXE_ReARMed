/*
 * MCU-compatible screen driver for PSX emulator
 * Compatible with NXP MIMXRT1052 microcontroller
 */

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

/* Native PSX pixel (mask, blue, green, red) to the RGB565 the panel wants. */
static void screen_repack_bgr555(const uint16_t *src, int32_t width, int32_t height, uint16_t *dst)
{
    for (int32_t y = 0; y < height; y++)
    {
        const uint16_t *s = src + (uint32_t)y * (PSX_GPU_FB_STRIDE / 2u);
        uint16_t *d = dst + (uint32_t)y * (uint32_t)width;

        for (int32_t x = 0; x < width; x++)
        {
            const uint32_t p = s[x];

            d[x] = (uint16_t)(((p & 0x1fu) << 11) | (((p >> 5) & 0x1fu) << 6) | ((p >> 10) & 0x1fu));
        }
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

static void screen_repack_rgb24(const uint8_t *src, int32_t width, int32_t height, uint16_t *dst)
{
    for (int32_t y = 0; y < height; y++)
    {
        const uint8_t *s = src + (uint32_t)y * PSX_GPU_FB_STRIDE;
        uint16_t *d = dst + (uint32_t)y * (uint32_t)width;

        for (int32_t x = 0; x < width; x++, s += 3)
            d[x] = (uint16_t)(((uint32_t)(s[0] & 0xf8u) << 8) | ((uint32_t)(s[1] & 0xfcu) << 3) |
                              ((uint32_t)s[2] >> 3));
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

static void psxe_screen_update_impl(psxe_screen_t *screen);

static uint32_t g_presented_frames = 0;

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

            PRINTF("emu: %u kcyc/s (%u%% of PS1) | vbl/s: %u | fps: %u | jit blk=%u cmp=%u flush=%u inv=%u code=%uB nat=%u int=%u\r\n",
                   kcyc, (unsigned)((kcyc * 100u) / 33869u), vblanks, g_presented_frames,
                   (unsigned)jit->blocks, (unsigned)jit->compiles, (unsigned)jit->flushes,
                   (unsigned)jit->invalidations, (unsigned)jit->code_used,
                   (unsigned)jit->native, (unsigned)jit->interp_steps);

            {
                /* Frames from the ESP32 pad bridge, so a wiring or baud rate
                   problem is visible without a debugger: frames climbing means
                   the link is alive, errors climbing means the line is noisy. */
                uint32_t pad_frames = 0;
                uint32_t pad_errors = 0;

                psxe_gamepad_get_stats(&pad_frames, &pad_errors);

    #if PSX_PROFILE
            PRINTF("mdec: idct=%u yuv=%u blocks=%u\r\n",
                   (unsigned)g_prof.mdec_idct, (unsigned)g_prof.mdec_yuv, (unsigned)g_prof.mdec_blk);
#endif

            PRINTF("disp: %dx%d %s mode=%03x yrange=%u..%u start=(%u,%u) | pad: %u/%u\r\n",
                       (int)psx_get_display_width(screen->psx),
                       (int)psx_get_display_height(screen->psx),
                       psx_get_display_format(screen->psx) ? "24bpp" : "15bpp",
                       (unsigned)screen->psx->gpu->display_mode,
                       (unsigned)screen->psx->gpu->disp_y1, (unsigned)screen->psx->gpu->disp_y2,
                       (unsigned)screen->psx->gpu->disp_x, (unsigned)screen->psx->gpu->disp_y,
                       (unsigned)pad_frames, (unsigned)pad_errors);
            }

#if PSX_JIT_HIST
            PRINTF("jit dispatch-interp=%u\r\n", (unsigned)jit->dispatch_steps);
#endif

            last_tick = now;
            last_cycles = cycles;
            vblanks = 0;
            g_presented_frames = 0;
        }
    }

    /* Nothing was drawn since the last presented frame: the LCD already
       shows this picture, so re-scaling it would be pure overhead. */
    if (!gpu->vram_dirty)
        return;

    gpu->vram_dirty = 0;

    PROF_T0(t_blit);
    PROF_INC(frames);
    psxe_screen_update_impl(screen);
    PROF_ADD(blit, t_blit);
}

static int32_t s_pxp_busy = 0;

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

static void psxe_screen_update_impl(psxe_screen_t *screen)
{
    static int32_t last_scaled_w = -1, last_scaled_h = -1;
    static int32_t clear_pending = 2;

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

        /* Both buffers have to lose the old letterbox bars */
        clear_pending = 2;

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
        if (is_24bpp)
            screen_repack_rgb24((const uint8_t *)src, src_width, src_height, g_rgb24_stage);
        else
            screen_repack_bgr555(src, src_width, src_height, g_rgb24_stage);
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
