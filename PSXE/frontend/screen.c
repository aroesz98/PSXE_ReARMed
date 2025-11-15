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

static uint32_t last_fps_time = 0;
void psxe_screen_update(psxe_screen_t *screen)
{
    static int32_t update_counter = 0;

    static uint32_t frame_count = 0;
    static float fps = 0.0f;

    update_counter++;

    uint32_t current_time = xTaskGetTickCount();
    frame_count++;

    void *display_buf = screen->debug_mode ? psx_get_vram(screen->psx) : psx_get_display_buffer(screen->psx);

    if ((screen->psx->gpu->disp_y + screen->texture_height) > 512)
        display_buf = psx_get_vram(screen->psx);

    uint16_t *src = (uint16_t *)display_buf;
    uint16_t *dst = (uint16_t *)DEMO_GetCurrentFrameBuffer(); // g_temp_framebuffer;

    if (!src)
    {
        return;
    }

    // Get actual PSX output dimensions dynamically with sanity checks
    int32_t src_width, src_height;
    if (screen->debug_mode)
    {
        src_width = PSX_GPU_FB_WIDTH;   // 640
        src_height = PSX_GPU_FB_HEIGHT; // 480
    }
    else
    {
        src_width = psx_get_display_width(screen->psx);
        src_height = psx_get_display_height(screen->psx);

        // Sanity check and limit PSX dimensions to reasonable values
        if (src_width <= 0 || src_width > 640)
        {
            src_width = 320; // Default PSX width
        }
        if (src_height <= 0 || src_height > 480)
        {
            src_height = 240; // Default PSX height
        }

        // Common PSX resolutions - fix weird values
        if (src_width == 320 && src_height > 240)
        {
            src_height = 240; // 320x240 is standard
        }
        if (src_width == 640 && src_height > 480)
        {
            src_height = 480; // 640x480 is max
        }

        // Log weird dimensions for debugging
        if (src_height > 300 && src_width < 400)
        {
            PRINTF("Warning: Unusual PSX dimensions %dx%d, limiting to safe values\r\n", src_width, src_height);
            src_width = 320;
            src_height = 240;
        }
    }

    // MCU display dimensions
    int32_t dst_width = LCD_WIDTH;   // 480
    int32_t dst_height = LCD_HEIGHT; // 272

    // Calculate scaling factors dynamically based on actual PSX resolution
    float scale_x = (float)dst_width / (float)src_width;
    float scale_y = (float)dst_height / (float)src_height;

    // Use the smaller scale to maintain aspect ratio (letterbox/pillarbox as needed)
    float scale = (scale_x < scale_y) ? scale_x : scale_y;

    int32_t scaled_width = (int)(src_width * scale);
    int32_t scaled_height = (int)(src_height * scale);

    // Center the scaled image on the display
    int32_t x_offset = (dst_width - scaled_width) / 2;
    int32_t y_offset = (dst_height - scaled_height) / 2;

    // Determine if we're downscaling or upscaling
    // Downscaling: PSX resolution is larger than LCD resolution
    // Upscaling: PSX resolution is smaller than LCD resolution
    int32_t is_downscaling = (src_width > dst_width) || (src_height > dst_height);
    int32_t use_pxp_scaling = is_downscaling;

    // Debug log occasionally
    static int32_t last_src_width = 0, last_src_height = 0;
    if (frame_count % 120 == 0 || src_width != last_src_width || src_height != last_src_height)
    {
        if (use_pxp_scaling) {
            PRINTF("PXP hardware scaling (downscale): %dx%d -> %dx%d, offset=(%d,%d)\r\n",
                   src_width, src_height, scaled_width, scaled_height, x_offset, y_offset);
            PRINTF("PSX buffer: 0x%08X, stride: %d, dst buffer: 0x%08X\r\n", 
                   (uint32_t)src, PSX_GPU_FB_STRIDE, (uint32_t)dst);
        } else {
            PRINTF("Software scaling (upscale): %dx%d -> %dx%d, offset=(%d,%d)\r\n",
                   src_width, src_height, scaled_width, scaled_height, x_offset, y_offset);
        }
        last_src_width = src_width;
        last_src_height = src_height;
    }

    // Clear the entire framebuffer to black first
    memset(dst, 0, dst_width * dst_height * sizeof(uint16_t));

    if (use_pxp_scaling)
    {
        // === Hardware PXP Scaling for Downscaling ===
        
        // Configure PXP Process Surface (input buffer)
        g_pxp_ps_config.bufferAddr = (uint32_t)src;
        // Use the correct PSX framebuffer stride - always use the full VRAM stride for PSX
        g_pxp_ps_config.pitchBytes = PSX_GPU_FB_STRIDE; // PSX VRAM stride in bytes
        PXP_SetProcessSurfaceBufferConfig(APP_PXP, &g_pxp_ps_config);

        // Configure PXP scaling: scale from source dimensions to output dimensions
        PXP_SetProcessSurfaceScaler(APP_PXP, src_width, src_height, dst_width, dst_height);
        
        // Set the source region (what part of the input buffer to use)
        // This defines the input area to be scaled - use full source area
        PXP_SetProcessSurfacePosition(APP_PXP, 0, 0, src_width - 1U, src_height - 1U);

        // Configure PXP output buffer - scale to full output size, we'll letterbox by clearing first
        g_pxp_output_config.buffer0Addr = (uint32_t)dst;
        g_pxp_output_config.width = dst_width;   // Full output width
        g_pxp_output_config.height = dst_height; // Full output height
        g_pxp_output_config.pitchBytes = dst_width * 2; // RGB565 = 2 bytes per pixel
        PXP_SetOutputBufferConfig(APP_PXP, &g_pxp_output_config);

        // Start PXP processing
        PXP_Start(APP_PXP);

        // Wait for PXP to complete processing
        while (!(kPXP_CompleteFlag & PXP_GetStatusFlags(APP_PXP)))
        {
            // Hardware scaling in progress
        }

        // Clear completion flag
        PXP_ClearStatusFlags(APP_PXP, kPXP_CompleteFlag);
    }
    else
    {
        // === Software Scaling for Upscaling ===
        
        // Pre-calculate source stride
        const int32_t src_stride = PSX_GPU_FB_STRIDE / 2;
        const int32_t max_src_idx = PSX_GPU_FB_STRIDE * PSX_GPU_FB_HEIGHT / 2;
        const int32_t max_dst_idx = dst_width * dst_height;

        // Use fixed-point arithmetic for better performance
        const int32_t scale_x_fp = (src_width << 16) / scaled_width;   // 16.16 fixed point
        const int32_t scale_y_fp = (src_height << 16) / scaled_height;

        for (int32_t dst_y = 0; dst_y < scaled_height; dst_y++)
        {
            // Calculate source Y using fixed-point
            const int32_t src_y = (dst_y * scale_y_fp) >> 16;
            const int32_t src_y_stride = src_y * src_stride;
            const int32_t dst_y_offset = (dst_y + y_offset) * dst_width + x_offset;

            for (int32_t dst_x = 0; dst_x < scaled_width; dst_x++)
            {
                // Calculate source X using fixed-point arithmetic
                const int32_t src_x = (dst_x * scale_x_fp) >> 16;
                const int32_t src_idx = src_y_stride + src_x;
                const int32_t dst_idx = dst_y_offset + dst_x;

                // Bounds checking
                if (src_idx < max_src_idx && dst_idx < max_dst_idx)
                {
                    dst[dst_idx] = src[src_idx];
                }
            }
        }
    }

    // Display the frame using buffer swap
    DEMO_SwapBuffers();

    // Calculate and display FPS every 1 second
    if (current_time - last_fps_time >= configTICK_RATE_HZ)
    {
        fps = (float)frame_count / ((current_time - last_fps_time) / (float)configTICK_RATE_HZ);
        PRINTF("FPS: %d\r\n", (int)fps);
        frame_count = 0;
        last_fps_time = current_time;
    }
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
