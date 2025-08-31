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

// Static buffer for screen instance
static psxe_screen_t g_screen_instance;
static int32_t g_screen_instance_used = 0;

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

// void psxe_screen_update(psxe_screen_t* screen) {
//     // Add simple frame rate limiting to prevent blinking caused by excessive updates
//     static int update_counter = 0;
//     update_counter++;

//     // Only update every 2nd or 3rd call to reduce refresh rate and prevent blinking
//     if (update_counter % 2 != 0) {
//         return; // Skip this update
//     }

//     void* display_buf = screen->debug_mode ?
//         psx_get_vram(screen->psx) : psx_get_display_buffer(screen->psx);

//     if ((screen->psx->gpu->disp_y + screen->texture_height) > 512)
//         display_buf = psx_get_vram(screen->psx);

//     // PSX output buffer is 640x480, we need to scale to 480x272
//     uint16_t* src = (uint16_t*)display_buf;
//     uint16_t* dst = (uint16_t*)DEMO_GetCurrentFrameBuffer(); // Use temporary buffer for scaling

//     // Validate display buffer before processing
//     if (!src || !dst) {
//         return; // Skip this frame if buffers are invalid
//     }

//     // PSX output dimensions (fixed)
//     int src_width = 640;
//     int src_height = 480;

//     // MCU display dimensions
//     int dst_width = LCD_WIDTH;   // 480
//     int dst_height = LCD_HEIGHT; // 272

//     // Debug log occasionally
//     static int frame_count = 0;
//     if (frame_count % 120 == 0) {
//         PRINTF("PSX scaling: %dx%d -> %dx%d, counter=%d\r\n",
//                src_width, src_height, dst_width, dst_height, update_counter);
//     }
//     frame_count++;

//     // Calculate scaling factors for 640x480 -> 480x272
//     // Scale factors: X = 480/640 = 0.75, Y = 272/480 = 0.567
//     float scale_y = 0.567f;   // 272/480 (approximately)

//     // Use Y scaling to fit height perfectly, X will be letterboxed
//     float scale = scale_y;    // Use smaller scale to fit entirely

//     int scaled_width = (int)(src_width * scale);   // 640 * 0.567 = 363
//     int scaled_height = (int)(src_height * scale); // 480 * 0.567 = 272

//     // Center the scaled image on the display
//     int x_offset = (dst_width - scaled_width) / 2;   // (480 - 363) / 2 = 58
//     int y_offset = (dst_height - scaled_height) / 2; // (272 - 272) / 2 = 0

//     // Clear the entire framebuffer to black
//     memset(dst, 0, dst_width * dst_height * sizeof(uint16_t));

//     // Scale the 640x480 PSX output to fit 480x272 display
//     for (int dst_y = 0; dst_y < scaled_height; dst_y++) {
//         for (int dst_x = 0; dst_x < scaled_width; dst_x++) {
//             // Map back to source coordinates in 640x480 buffer
//             int src_x = (int)((float)dst_x / scale);
//             int src_y = (int)((float)dst_y / scale);

//             if (src_x < src_width && src_y < src_height) {
//                 // Calculate source index in 640x480 buffer
//                 int src_idx = src_y * (PSX_GPU_FB_STRIDE / 2) + src_x;
//                 // Calculate destination index in 480x272 buffer
//                 int dst_idx = (dst_y + y_offset) * dst_width + (dst_x + x_offset);

//                 // Bounds checking
//                 if (src_idx >= 0 && src_idx < (PSX_GPU_FB_STRIDE * PSX_GPU_FB_HEIGHT / 2) &&
//                     dst_idx >= 0 && dst_idx < (dst_width * dst_height)) {

//                     // Convert BGR555 to RGB565 for MCU display
//                     uint16_t bgr555 = src[src_idx];
//                     uint16_t b = (bgr555 >> 10) & 0x1F;  // Blue: bits 14-10
//                     uint16_t g = (bgr555 >> 5) & 0x1F;   // Green: bits 9-5
//                     uint16_t r = (bgr555 >> 0) & 0x1F;   // Red: bits 4-0

//                     // Convert to RGB565: extend green from 5 to 6 bits
//                     dst[dst_idx] = (r << 11) | (g << 6) | b;
//                 }
//             }
//         }
//     }

//     // Copy the scaled image from temporary buffer to current framebuffer
// //    uint8_t* current_framebuffer = DEMO_GetCurrentFrameBuffer();
// //    memcpy(current_framebuffer, g_temp_framebuffer, dst_width * dst_height * sizeof(uint16_t));

//     // Display the frame using buffer swap
//     DEMO_SwapBuffers();

//     // Handle input from GPIO buttons (simplified to avoid type issues)
//     // TODO: Re-implement proper button handling later
//     // uint32_t current_buttons = screen_get_button_from_gpio();
//     // uint32_t pressed_buttons = current_buttons & ~screen->prev_button_state;
//     // uint32_t released_buttons = screen->prev_button_state & ~current_buttons;

//     // Update button states
//     // if (pressed_buttons) {
//     //     psx_pad_button_press(screen->pad, 0, pressed_buttons);
//     // }
//     // if (released_buttons) {
//     //     psx_pad_button_release(screen->pad, 0, released_buttons);
//     // }

//     // screen->prev_button_state = current_buttons;
// }
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

    // Debug log occasionally
    static int32_t last_src_width = 0, last_src_height = 0;
    if (frame_count % 120 == 0 || src_width != last_src_width || src_height != last_src_height)
    {
        PRINTF("Software scaling: %dx%d -> %dx%d, offset=(%d,%d)\r\n",
               src_width, src_height, scaled_width, scaled_height, x_offset, y_offset);
        last_src_width = src_width;
        last_src_height = src_height;
    }

    // Clear the entire framebuffer to black
    memset(dst, 0, dst_width * dst_height * sizeof(uint16_t));

    // Scale the PSX output to fit the MCU display with optimized scaling
    // Pre-calculate fixed-point scale factors to avoid float division in loops
    const int32_t scale_x_fp = (int32_t)((float)dst_width * 65536.0f / (float)src_width); // 16.16 fixed point
    const int32_t scale_y_fp = (int32_t)((float)dst_height * 65536.0f / (float)src_height);
    const int32_t scale_fp = (scale_x_fp < scale_y_fp) ? scale_x_fp : scale_y_fp;

    // Use the smaller scale to maintain aspect ratio
    const int32_t actual_scaled_width = (src_width * scale_fp) >> 16;
    const int32_t actual_scaled_height = (src_height * scale_fp) >> 16;
    const int32_t x_offset_opt = (dst_width - actual_scaled_width) / 2;
    const int32_t y_offset_opt = (dst_height - actual_scaled_height) / 2;

    // Pre-calculate source stride
    const int32_t src_stride = PSX_GPU_FB_STRIDE / 2;
    const int32_t max_src_idx = PSX_GPU_FB_STRIDE * PSX_GPU_FB_HEIGHT / 2;
    const int32_t max_dst_idx = dst_width * dst_height;

    for (int32_t dst_y = 0; dst_y < actual_scaled_height; dst_y++)
    {
        // Calculate source Y once per row using fixed-point
        const int32_t src_y = (dst_y * 65536) / scale_fp;
        const int32_t src_y_stride = src_y * src_stride;
        const int32_t dst_y_offset = (dst_y + y_offset_opt) * dst_width + x_offset_opt;

        for (int32_t dst_x = 0; dst_x < actual_scaled_width; dst_x++)
        {
            // Calculate source X using fixed-point arithmetic
            const int32_t src_x = (dst_x * 65536) / scale_fp;
            const int32_t src_idx = src_y_stride + src_x;
            const int32_t dst_idx = dst_y_offset + dst_x;

            // Simplified bounds checking
            if (src_idx < max_src_idx && dst_idx < max_dst_idx)
            {
                dst[dst_idx] = src[src_idx];
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
