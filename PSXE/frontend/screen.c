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

            PRINTF("emu: %u kcyc/s (%u%% of PS1) | vblanks/s: %u | frames/s: %u\r\n",
                   kcyc, (unsigned)((kcyc * 100u) / 33869u), vblanks, g_presented_frames);

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

    if ((screen->psx->gpu->disp_y + screen->texture_height) > 512)
        display_buf = psx_get_vram(screen->psx);

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
    }

    const int32_t dst_width = LCD_WIDTH;   /* 480 */
    const int32_t dst_height = LCD_HEIGHT; /* 272 */

    /* Fit into the panel keeping the aspect ratio (integer math only) */
    int32_t scaled_height = dst_height;
    int32_t scaled_width = (src_width * dst_height) / src_height;

    if (scaled_width > dst_width)
    {
        scaled_width = dst_width;
        scaled_height = (src_height * dst_width) / src_width;
    }

    const int32_t x_offset = (dst_width - scaled_width) / 2;
    const int32_t y_offset = (dst_height - scaled_height) / 2;

    uint16_t *dst = (uint16_t *)DEMO_GetCurrentFrameBuffer();

    if ((scaled_width != last_scaled_w) || (scaled_height != last_scaled_h))
    {
        last_scaled_w = scaled_width;
        last_scaled_h = scaled_height;

        /* Both buffers have to lose the old letterbox bars */
        clear_pending = 2;

        PRINTF("PXP scaling: %dx%d -> %dx%d, offset=(%d,%d)\r\n",
               src_width, src_height, scaled_width, scaled_height, x_offset, y_offset);
    }

    if (clear_pending > 0)
    {
        clear_pending--;

        memset(dst, 0, dst_width * dst_height * sizeof(uint16_t));
    }

    /* The rasterizer writes VRAM through the D-cache, PXP reads it as a bus
       master - a full clean is cheaper than cleaning the display window. */
    SCB_CleanDCache();

    /* Process surface = PSX display area inside VRAM */
    g_pxp_ps_config.bufferAddr = (uint32_t)src;
    g_pxp_ps_config.pitchBytes = PSX_GPU_FB_STRIDE;
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
