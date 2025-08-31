/*
 * Copyright 2019-2021, 2023 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "display_support.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#if defined(SDK_OS_FREE_RTOS)
#include "FreeRTOS.h"
#include "semphr.h"
#endif

#include "board.h"
#include "fsl_device_registers.h"
#include "MIMXRT1052.h"
#include "fsl_common_arm.h"
//#include "fsl_video_common.h"
#include "fsl_elcdif.h"
#include "fsl_gpio.h"
#include "fsl_cache.h"
#include "fsl_debug_console.h"
#include "fsl_pxp.h"
#include "core_cm7.h"

// Suppress warnings for static functions that may not be used
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

// Enable PXP for hardware acceleration
#define DISABLE_PXP 0

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define LCD_HSW 40
#define LCD_HFP 16
#define LCD_HBP 56
#define LCD_VSW 10
#define LCD_VFP 3
#define LCD_VBP 8

#define LCD_POL_FLAGS \
    (kELCDIF_DataEnableActiveHigh | kELCDIF_VsyncActiveLow | kELCDIF_HsyncActiveLow | kELCDIF_DriveDataOnRisingClkEdge)

#define LCD_FB_BYTE_PER_PIXEL 2
#define LCDIF_PIXEL_FORMAT    kELCDIF_PixelFormatRGB565
#define LCD_LCDIF_DATA_BUS    kELCDIF_DataBus16Bit

/* Back light. */
#define LCD_BL_GPIO     GPIO2
#define LCD_BL_GPIO_PIN 31

/* Cache line size. */
#ifndef FSL_FEATURE_L2CACHE_LINESIZE_BYTE
#define FSL_FEATURE_L2CACHE_LINESIZE_BYTE 0
#endif
#ifndef FSL_FEATURE_L1DCACHE_LINESIZE_BYTE
#define FSL_FEATURE_L1DCACHE_LINESIZE_BYTE 0
#endif

#if (FSL_FEATURE_L2CACHE_LINESIZE_BYTE > FSL_FEATURE_L1DCACHE_LINESIZE_BYTE)
#define DEMO_CACHE_LINE_SIZE FSL_FEATURE_L2CACHE_LINESIZE_BYTE
#else
#define DEMO_CACHE_LINE_SIZE FSL_FEATURE_L1DCACHE_LINESIZE_BYTE
#endif

#if (DEMO_CACHE_LINE_SIZE > FRAME_BUFFER_ALIGN)
#define DEMO_FB_ALIGN DEMO_CACHE_LINE_SIZE
#else
#define DEMO_FB_ALIGN FRAME_BUFFER_ALIGN
#endif

#if (LV_ATTRIBUTE_MEM_ALIGN_SIZE > DEMO_FB_ALIGN)
#undef DEMO_FB_ALIGN
#define DEMO_FB_ALIGN LV_ATTRIBUTE_MEM_ALIGN_SIZE
#endif

#define DEMO_FB_SIZE (((LCD_WIDTH * LCD_HEIGHT * LCD_FB_BYTE_PER_PIXEL) + DEMO_FB_ALIGN - 1) & ~(DEMO_FB_ALIGN - 1))

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void DEMO_InitLcdClock(void);

static void DEMO_InitLcdBackLight(void);

void DEMO_DisplayColorTest(uint16_t background_color);

void DEMO_DisplayColorTest_WithColor(uint16_t background_color);

void DEMO_DrawRectangle(int x, int y, int w, int h, uint16_t color);

void DEMO_SwapBuffers(void); // Made non-static for external access

// Text rendering functions
void DEMO_DrawText(int x, int y, const char* text, uint16_t color);
void DEMO_DrawNumber(int x, int y, int number, uint16_t color);

// Convenience functions with buffer swapping for standalone usage
void DEMO_DisplayColorTest_AndSwap(uint16_t background_color);
void DEMO_DrawRectangle_AndSwap(int x, int y, int w, int h, uint16_t color);
void DEMO_Draw3DCube_AndSwap(int center_x, int center_y, int size, float rotation_x, float rotation_y, float rotation_z, uint16_t color);

/*******************************************************************************
 * Variables
 ******************************************************************************/
static volatile bool s_framePending;
static volatile int s_currentFrontBuffer = 1;  // Currently displayed buffer (0 or 1)
static volatile int s_currentBackBuffer = 0;   // Currently drawing buffer (0 or 1)
static volatile bool s_bufferSwapRequested = false;  // Buffer swap pending flag
#if defined(SDK_OS_FREE_RTOS)
static SemaphoreHandle_t s_frameSema;
#endif

// Simplified frame buffer declaration for testing
__attribute__((aligned(32), section(".bss.$BOARD_SDRAM"))) static uint8_t s_frameBuffer[2][DEMO_FB_SIZE];

/*******************************************************************************
 * PXP Support - Hardware Acceleration
 ******************************************************************************/
static bool s_pxpInitialized = false;

// Initialize PXP for hardware acceleration
static bool PXP_Init_Fixed(void)
{
    if (s_pxpInitialized)
        return true;
        
    // Enable PXP clock
    CCM->CCGR2 |= CCM_CCGR2_CG5(3); // Enable PXP clock
    
    // Initialize PXP
    PXP_Init(PXP);
    
    // Reset PXP to clear any previous state
    PXP_Reset(PXP);
    
    s_pxpInitialized = true;
    return true;
}

// Wait for PXP operation with timeout to prevent hanging
static bool PXP_WaitCompletionWithTimeout(void)
{
    uint32_t timeout = 100000; // Timeout counter
    
    while (!(PXP_GetStatusFlags(PXP) & kPXP_CompleteFlag) && timeout > 0)
    {
        timeout--;
    }
    
    if (timeout == 0)
    {
        // Timeout - reset PXP and disable for safety
        PXP_Reset(PXP);
        return false;
    }
    
    // Clear completion flag
    PXP_ClearStatusFlags(PXP, kPXP_CompleteFlag);
    return true;
}

// Fast rectangle fill using PXP
static bool PXP_FillRectangle(int x, int y, int width, int height, uint16_t color)
{
    if (!PXP_Init_Fixed())
        return false;
        
    // Bounds checking
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return false;
        
    if (width <= 0 || height <= 0)
        return false;
        
    // Clip rectangle to screen boundaries
    if (x + width > LCD_WIDTH)
        width = LCD_WIDTH - x;
        
    if (y + height > LCD_HEIGHT)
        height = LCD_HEIGHT - y;
    
    // For partial rectangle fills, we need to handle this differently
    // PXP_BuildRect fills entire buffer, so for sub-rectangles we'll use CPU fallback
    if (x != 0 || y != 0 || width != LCD_WIDTH || height != LCD_HEIGHT)
    {
        return false; // Use CPU fallback for partial rectangles
    }
    
    // Use PXP_BuildRect for full screen fills only
    // For RGB565, just use the color directly
    PXP_BuildRect(PXP, kPXP_OutputPixelFormatRGB565, color, LCD_WIDTH, LCD_HEIGHT, 
                  LCD_WIDTH * 2, (uint32_t)s_frameBuffer[s_currentBackBuffer]);
    
    // Wait for completion
    return PXP_WaitCompletionWithTimeout();
}

/*
// Fast full-screen buffer clear using PXP
static bool PXP_ClearScreen(uint16_t color)
{
    // Use the rectangle fill for full screen
    return PXP_FillRectangle(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}
*/

/*******************************************************************************
 * Code
 ******************************************************************************/

void lv_port_pre_init(void)
{
}

void lv_port_disp_init(void)
{
    DEMO_InitLcd();
}

void LCDIF_IRQHandler(void)
{
#if defined(SDK_OS_FREE_RTOS)
    BaseType_t taskAwake = pdFALSE;
#endif

    uint32_t intStatus = ELCDIF_GetInterruptStatus(LCDIF);

    ELCDIF_ClearInterruptStatus(LCDIF, intStatus);

    if (s_framePending)
    {
        if (intStatus & kELCDIF_CurFrameDone)
        {
            s_framePending = false;
            
            // Handle buffer swap if requested
            if (s_bufferSwapRequested)
            {
                // Swap buffers
                int temp = s_currentFrontBuffer;
                s_currentFrontBuffer = s_currentBackBuffer;
                s_currentBackBuffer = temp;
                s_bufferSwapRequested = false;
            }

#if defined(SDK_OS_FREE_RTOS)
            xSemaphoreGiveFromISR(s_frameSema, &taskAwake);

            portYIELD_FROM_ISR(taskAwake);
#endif
        }
    }
    SDK_ISR_EXIT_BARRIER;
}

static void DEMO_InitLcdClock(void)
{
    /*
     * The desired output frame rate is 60Hz. So the pixel clock frequency is:
     * (480 + 41 + 4 + 18) * (272 + 10 + 4 + 2) * 60 = 9.2M.
     * Here set the LCDIF pixel clock to 9.3M.
     */

    /*
     * Initialize the Video PLL.
     * Video PLL output clock is OSC24M * (loopDivider + (denominator / numerator)) / postDivider = 93MHz.
     */
    clock_video_pll_config_t config = {
        .loopDivider = 36,
        .postDivider = 8,
        .numerator   = 0,
        .denominator = 0,
    };

    CLOCK_InitVideoPll(&config);

    /*
     * 000 derive clock from PLL2
     * 001 derive clock from PLL3 PFD3
     * 010 derive clock from PLL5
     * 011 derive clock from PLL2 PFD0
     * 100 derive clock from PLL2 PFD1
     * 101 derive clock from PLL3 PFD1
     */
    CLOCK_SetMux(kCLOCK_LcdifPreMux, 2);

    CLOCK_SetDiv(kCLOCK_LcdifPreDiv, 2);

    CLOCK_SetDiv(kCLOCK_LcdifDiv, 1);
}

static void DEMO_InitLcdBackLight(void)
{
    const gpio_pin_config_t config = {
        kGPIO_DigitalOutput,
        1,
        kGPIO_NoIntmode,
    };

    /* Backlight. */
    GPIO_PinInit(LCD_BL_GPIO, LCD_BL_GPIO_PIN, &config);
}

void DEMO_InitLcd(void)
{
    /* Initialize the display. */
    const elcdif_rgb_mode_config_t config = {
        .panelWidth    = LCD_WIDTH,
        .panelHeight   = LCD_HEIGHT,
        .hsw           = LCD_HSW,
        .hfp           = LCD_HFP,
        .hbp           = LCD_HBP,
        .vsw           = LCD_VSW,
        .vfp           = LCD_VFP,
        .vbp           = LCD_VBP,
        .polarityFlags = LCD_POL_FLAGS,
        /* lvgl starts render in frame buffer 0, so show frame buffer 1 first. */
        .bufferAddr  = (uint32_t)s_frameBuffer[1],
        .pixelFormat = LCDIF_PIXEL_FORMAT,
        .dataBus     = LCD_LCDIF_DATA_BUS,
    };

    /* Clear frame buffer. */
    memset((void *)s_frameBuffer, 0, sizeof(s_frameBuffer));

#if defined(SDK_OS_FREE_RTOS)
    s_frameSema = xSemaphoreCreateBinary();
    if (NULL == s_frameSema)
    {
        PRINTF("Frame semaphore create failed\r\n");
        assert(0);
    }
#endif

    /* No frame pending. */
    s_framePending = false;
#if defined(SDK_OS_FREE_RTOS)
    NVIC_SetPriority(LCDIF_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1);
#endif

    DEMO_InitLcdClock();

    ELCDIF_RgbModeInit(LCDIF, &config);

    ELCDIF_EnableInterrupts(LCDIF, kELCDIF_CurFrameDoneInterruptEnable);
    NVIC_EnableIRQ(LCDIF_IRQn);
    ELCDIF_RgbModeStart(LCDIF);

    DEMO_InitLcdBackLight();
}

// Double buffer management to prevent tearing
void DEMO_SwapBuffers(void)
{
    // Wait for any pending frame to complete
    while (s_framePending)
    {
    }
    
    // Clean the back buffer cache before displaying
    DCACHE_CleanInvalidateByRange((uint32_t)s_frameBuffer[s_currentBackBuffer], DEMO_FB_SIZE);
    
    // Set the back buffer as the new display buffer
    ELCDIF_SetNextBufferAddr(LCDIF, (uint32_t)s_frameBuffer[s_currentBackBuffer]);
    
    // Request buffer swap (will happen in interrupt handler when frame is done)
    s_bufferSwapRequested = true;
    
    // Mark frame as pending
    s_framePending = true;
    
#if defined(SDK_OS_FREE_RTOS)
    // Wait for vsync (frame completion)
    if (xSemaphoreTake(s_frameSema, portMAX_DELAY) == pdTRUE)
    {
        // Frame swap completed
    }
    else
    {
        PRINTF("Buffer swap failed\r\n");
    }
#else
    // Wait for vsync (frame completion)
    while (s_framePending)
    {
    }
#endif
}

void DEMO_DisplayColorTest(uint16_t background_color)
{
    // Use smart PXP acceleration - only beneficial for full screen clears
    if (PXP_SmartClearScreen(background_color))
    {
        return; // PXP succeeded
    }
    
    // Fallback to optimized CPU method
    uint16_t *buffer = (uint16_t *)s_frameBuffer[s_currentBackBuffer];
    
    // Use optimized approach: fill first few pixels, then use memcpy to replicate
    const int total_pixels = LCD_WIDTH * LCD_HEIGHT;
    const int chunk_size = 1024; // Fill 1024 pixels at once
    
    // Fill the first chunk manually
    int pixels_to_fill = (total_pixels < chunk_size) ? total_pixels : chunk_size;
    for (int i = 0; i < pixels_to_fill; i++)
    {
        buffer[i] = background_color;
    }
    
    // Use memcpy to replicate the pattern for the rest of the buffer
    int pixels_filled = pixels_to_fill;
    while (pixels_filled < total_pixels)
    {
        int copy_size = ((total_pixels - pixels_filled) > pixels_filled) ? 
                        pixels_filled : (total_pixels - pixels_filled);
        memcpy(&buffer[pixels_filled], buffer, copy_size * sizeof(uint16_t));
        pixels_filled += copy_size;
    }
}

void DEMO_DrawRectangle(int x, int y, int w, int h, uint16_t color)
{
    // Try PXP acceleration first for rectangles
    if (PXP_FillRectAccel(x, y, w, h, color))
    {
        return; // PXP succeeded
    }
    
    // Fallback to CPU implementation
    // Bounds checking
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    
    if (w <= 0 || h <= 0)
        return;
    
    // Clip rectangle to screen boundaries
    if (x + w > LCD_WIDTH)
        w = LCD_WIDTH - x;
    
    if (y + h > LCD_HEIGHT)
        h = LCD_HEIGHT - y;
    
    uint16_t *buffer = (uint16_t *)s_frameBuffer[s_currentBackBuffer];
    
    // Draw rectangle line by line for better cache performance
    for (int row = 0; row < h; row++)
    {
        int line_start = (y + row) * LCD_WIDTH + x;
        
        // Fill the entire row of the rectangle
        for (int col = 0; col < w; col++)
        {
            buffer[line_start + col] = color;
        }
    }
    
    // Note: No buffer swap here - let caller decide when to swap
}

/*******************************************************************************
 * Additional PXP Accelerated Functions
 ******************************************************************************/

// PXP accelerated horizontal line drawing
bool PXP_DrawHorizontalLine(int x, int y, int width, uint16_t color)
{
    return PXP_FillRectangle(x, y, width, 1, color);
}

// PXP accelerated vertical line drawing
bool PXP_DrawVerticalLine(int x, int y, int height, uint16_t color)
{
    return PXP_FillRectangle(x, y, 1, height, color);
}

// PXP accelerated buffer copy operation
bool PXP_CopyBuffer(void *src_buffer, int src_x, int src_y, int src_width, int src_height, 
                   int dest_x, int dest_y, int copy_width, int copy_height)
{
    if (!PXP_Init_Fixed())
        return false;
        
    // Bounds checking for destination
    if (dest_x < 0 || dest_y < 0 || dest_x >= LCD_WIDTH || dest_y >= LCD_HEIGHT)
        return false;
        
    if (copy_width <= 0 || copy_height <= 0)
        return false;
        
    // Clip to screen boundaries
    if (dest_x + copy_width > LCD_WIDTH)
        copy_width = LCD_WIDTH - dest_x;
        
    if (dest_y + copy_height > LCD_HEIGHT)
        copy_height = LCD_HEIGHT - dest_y;
    
    // Use PXP's picture copy function
    pxp_pic_copy_config_t copyConfig;
    
    // Source configuration
    copyConfig.srcPicBaseAddr = (uint32_t)src_buffer;
    copyConfig.srcPitchBytes = src_width * 2;
    copyConfig.srcOffsetX = src_x;
    copyConfig.srcOffsetY = src_y;
    
    // Destination configuration
    copyConfig.destPicBaseAddr = (uint32_t)s_frameBuffer[s_currentBackBuffer];
    copyConfig.destPitchBytes = LCD_WIDTH * 2;
    copyConfig.destOffsetX = dest_x;
    copyConfig.destOffsetY = dest_y;
    copyConfig.width = copy_width;
    copyConfig.height = copy_height;
    copyConfig.pixelFormat = kPXP_AsPixelFormatRGB565;
    
    // Start copy operation
    if (PXP_StartPictureCopy(PXP, &copyConfig) != kStatus_Success)
        return false;
    
    // Wait for completion
    return PXP_WaitCompletionWithTimeout();
}

// PXP accelerated buffer copy with scaling
bool PXP_CopyBufferWithScale(void *src_buffer, int src_width, int src_height,
                            int dest_x, int dest_y, int dest_width, int dest_height)
{
    if (!PXP_Init_Fixed())
        return false;
        
    // Bounds checking for destination
    if (dest_x < 0 || dest_y < 0 || dest_x >= LCD_WIDTH || dest_y >= LCD_HEIGHT)
        return false;
        
    if (dest_width <= 0 || dest_height <= 0)
        return false;
        
    // Clip to screen boundaries
    if (dest_x + dest_width > LCD_WIDTH)
        dest_width = LCD_WIDTH - dest_x;
        
    if (dest_y + dest_height > LCD_HEIGHT)
        dest_height = LCD_HEIGHT - dest_y;
    
    // Use PXP's picture copy function with scaling
    pxp_pic_copy_config_t copyConfig;
    
    // Source configuration
    copyConfig.srcPicBaseAddr = (uint32_t)src_buffer;
    copyConfig.srcPitchBytes = src_width * 2;
    copyConfig.srcOffsetX = 0;
    copyConfig.srcOffsetY = 0;
    
    // Destination configuration (scaled)
    copyConfig.destPicBaseAddr = (uint32_t)s_frameBuffer[s_currentBackBuffer];
    copyConfig.destPitchBytes = LCD_WIDTH * 2;
    copyConfig.destOffsetX = dest_x;
    copyConfig.destOffsetY = dest_y;
    copyConfig.width = dest_width;
    copyConfig.height = dest_height;
    copyConfig.pixelFormat = kPXP_AsPixelFormatRGB565;
    
    // Start copy operation (PXP will handle scaling automatically)
    if (PXP_StartPictureCopy(PXP, &copyConfig) != kStatus_Success)
        return false;
    
    // Wait for completion
    return PXP_WaitCompletionWithTimeout();
}

// Simplified rotation function using memory copy (PXP rotation is complex)
bool PXP_RotateBuffer(void *src_buffer, int src_width, int src_height,
                     int dest_x, int dest_y, pxp_rotate_degree_t rotation)
{
    // For now, just copy without rotation (rotation requires more complex setup)
    // This can be enhanced later with proper rotation support
    if (rotation == kPXP_Rotate0)
    {
        return PXP_CopyBufferWithScale(src_buffer, src_width, src_height, 
                                      dest_x, dest_y, src_width, src_height);
    }
    
    // For other rotations, fall back to CPU implementation or return false
    return false;
}

// High-level convenience functions using PXP acceleration

// RGB565 color blending helper functions
static uint16_t blend_rgb565(uint16_t fg_color, uint16_t bg_color, float alpha)
{
    if (alpha >= 1.0f) return fg_color;
    if (alpha <= 0.0f) return bg_color;
    
    // Extract RGB components from RGB565
    uint8_t fg_r = (fg_color >> 11) & 0x1F;
    uint8_t fg_g = (fg_color >> 5) & 0x3F;
    uint8_t fg_b = fg_color & 0x1F;
    
    uint8_t bg_r = (bg_color >> 11) & 0x1F;
    uint8_t bg_g = (bg_color >> 5) & 0x3F;
    uint8_t bg_b = bg_color & 0x1F;
    
    // Blend components
    uint8_t result_r = (uint8_t)(fg_r * alpha + bg_r * (1.0f - alpha));
    uint8_t result_g = (uint8_t)(fg_g * alpha + bg_g * (1.0f - alpha));
    uint8_t result_b = (uint8_t)(fg_b * alpha + bg_b * (1.0f - alpha));
    
    // Pack back to RGB565
    return (result_r << 11) | (result_g << 5) | result_b;
}

static void set_pixel_aa(int x, int y, uint16_t color, float alpha)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT)
        return;
        
    uint16_t *buffer = (uint16_t *)s_frameBuffer[s_currentBackBuffer];
    uint16_t bg_color = buffer[y * LCD_WIDTH + x];
    buffer[y * LCD_WIDTH + x] = blend_rgb565(color, bg_color, alpha);
}

static float fpart(float x) 
{
    return x - floorf(x);
}

static float rfpart(float x) 
{
    return 1.0f - fpart(x);
}

// Anti-aliased line drawing using Xiaolin Wu's algorithm
static void DEMO_DrawLine(int x0, int y0, int x1, int y1, uint16_t color)
{
    // Convert to float for sub-pixel precision
    float fx0 = (float)x0, fy0 = (float)y0;
    float fx1 = (float)x1, fy1 = (float)y1;
    
    int steep = fabsf(fy1 - fy0) > fabsf(fx1 - fx0);
    
    if (steep) 
    {
        // Swap x and y coordinates
        float temp = fx0; fx0 = fy0; fy0 = temp;
        temp = fx1; fx1 = fy1; fy1 = temp;
    }
    
    if (fx0 > fx1) 
    {
        // Swap start and end points
        float temp = fx0; fx0 = fx1; fx1 = temp;
        temp = fy0; fy0 = fy1; fy1 = temp;
    }
    
    float dx = fx1 - fx0;
    float dy = fy1 - fy0;
    float gradient = (dx == 0.0f) ? 1.0f : dy / dx;
    
    // Handle first endpoint
    float xend = roundf(fx0);
    float yend = fy0 + gradient * (xend - fx0);
    float xgap = rfpart(fx0 + 0.5f);
    int xpxl1 = (int)xend;
    int ypxl1 = (int)floorf(yend);
    
    if (steep) 
    {
        set_pixel_aa(ypxl1, xpxl1, color, rfpart(yend) * xgap);
        set_pixel_aa(ypxl1 + 1, xpxl1, color, fpart(yend) * xgap);
    } 
    else 
    {
        set_pixel_aa(xpxl1, ypxl1, color, rfpart(yend) * xgap);
        set_pixel_aa(xpxl1, ypxl1 + 1, color, fpart(yend) * xgap);
    }
    
    float intery = yend + gradient;
    
    // Handle second endpoint
    xend = roundf(fx1);
    yend = fy1 + gradient * (xend - fx1);
    xgap = fpart(fx1 + 0.5f);
    int xpxl2 = (int)xend;
    int ypxl2 = (int)floorf(yend);
    
    if (steep) 
    {
        set_pixel_aa(ypxl2, xpxl2, color, rfpart(yend) * xgap);
        set_pixel_aa(ypxl2 + 1, xpxl2, color, fpart(yend) * xgap);
    } 
    else 
    {
        set_pixel_aa(xpxl2, ypxl2, color, rfpart(yend) * xgap);
        set_pixel_aa(xpxl2, ypxl2 + 1, color, fpart(yend) * xgap);
    }
    
    // Main loop - draw the line between the endpoints
    if (steep) 
    {
        for (int x = xpxl1 + 1; x < xpxl2; x++) 
        {
            set_pixel_aa((int)floorf(intery), x, color, rfpart(intery));
            set_pixel_aa((int)floorf(intery) + 1, x, color, fpart(intery));
            intery += gradient;
        }
    } 
    else 
    {
        for (int x = xpxl1 + 1; x < xpxl2; x++) 
        {
            set_pixel_aa(x, (int)floorf(intery), color, rfpart(intery));
            set_pixel_aa(x, (int)floorf(intery) + 1, color, fpart(intery));
            intery += gradient;
        }
    }
}

// Manual string parsing functions to avoid sscanf (which can cause hardfaults on some MCUs)
static int skip_whitespace(const char **ptr, const char *end)
{
    while (*ptr < end && (**ptr == ' ' || **ptr == '\t')) (*ptr)++;
    return (*ptr < end);
}

static int parse_float(const char **ptr, const char *end, float *result)
{
    if (!skip_whitespace(ptr, end)) return 0;
    
    float sign = 1.0f;
    float value = 0.0f;
    float decimal = 0.1f;
    
    // Handle negative sign
    if (*ptr < end && **ptr == '-') {
        sign = -1.0f;
        (*ptr)++;
    } else if (*ptr < end && **ptr == '+') {
        (*ptr)++;
    }
    
    // Parse integer part
    while (*ptr < end && **ptr >= '0' && **ptr <= '9') {
        value = value * 10.0f + (**ptr - '0');
        (*ptr)++;
    }
    
    // Parse decimal part
    if (*ptr < end && **ptr == '.') {
        (*ptr)++;
        while (*ptr < end && **ptr >= '0' && **ptr <= '9') {
            value += decimal * (**ptr - '0');
            decimal *= 0.1f;
            (*ptr)++;
        }
    }
    
    *result = sign * value;
    return 1;
}

static int parse_int(const char **ptr, const char *end, int *result)
{
    if (!skip_whitespace(ptr, end)) return 0;
    
    int sign = 1;
    int value = 0;
    
    // Handle negative sign
    if (*ptr < end && **ptr == '-') {
        sign = -1;
        (*ptr)++;
    } else if (*ptr < end && **ptr == '+') {
        (*ptr)++;
    }
    
    // Parse digits
    if (*ptr >= end || **ptr < '0' || **ptr > '9') return 0;
    
    while (*ptr < end && **ptr >= '0' && **ptr <= '9') {
        value = value * 10 + (**ptr - '0');
        (*ptr)++;
    }
    
    *result = sign * value;
    return 1;
}

static int parse_vertex_line(const char *line, const char *line_end, float *x, float *y, float *z)
{
    const char *ptr = line + 2; // Skip "v "
    return parse_float(&ptr, line_end, x) && 
           parse_float(&ptr, line_end, y) && 
           parse_float(&ptr, line_end, z);
}

static int parse_texcoord_line(const char *line, const char *line_end, float *u, float *v)
{
    const char *ptr = line + 3; // Skip "vt "
    return parse_float(&ptr, line_end, u) && 
           parse_float(&ptr, line_end, v);
}

static int parse_normal_line(const char *line, const char *line_end, float *x, float *y, float *z)
{
    const char *ptr = line + 3; // Skip "vn "
    return parse_float(&ptr, line_end, x) && 
           parse_float(&ptr, line_end, y) && 
           parse_float(&ptr, line_end, z);
}

static int parse_face_line_full(const char *line, const char *line_end, 
                                int *v1, int *vt1, int *vn1,
                                int *v2, int *vt2, int *vn2,
                                int *v3, int *vt3, int *vn3)
{
    const char *ptr = line + 2; // Skip "f "
    
    // Parse first vertex (v1/vt1/vn1)
    if (!parse_int(&ptr, line_end, v1)) return 0;
    if (ptr >= line_end || *ptr != '/') return 0;
    ptr++;
    if (!parse_int(&ptr, line_end, vt1)) return 0;
    if (ptr >= line_end || *ptr != '/') return 0;
    ptr++;
    if (!parse_int(&ptr, line_end, vn1)) return 0;
    
    // Parse second vertex (v2/vt2/vn2)
    if (!parse_int(&ptr, line_end, v2)) return 0;
    if (ptr >= line_end || *ptr != '/') return 0;
    ptr++;
    if (!parse_int(&ptr, line_end, vt2)) return 0;
    if (ptr >= line_end || *ptr != '/') return 0;
    ptr++;
    if (!parse_int(&ptr, line_end, vn2)) return 0;
    
    // Parse third vertex (v3/vt3/vn3)
    if (!parse_int(&ptr, line_end, v3)) return 0;
    if (ptr >= line_end || *ptr != '/') return 0;
    ptr++;
    if (!parse_int(&ptr, line_end, vt3)) return 0;
    if (ptr >= line_end || *ptr != '/') return 0;
    ptr++;
    if (!parse_int(&ptr, line_end, vn3)) return 0;
    
    return 1;
}

static int parse_face_line_simple(const char *line, const char *line_end, int *v1, int *v2, int *v3)
{
    const char *ptr = line + 2; // Skip "f "
    return parse_int(&ptr, line_end, v1) && 
           parse_int(&ptr, line_end, v2) && 
           parse_int(&ptr, line_end, v3);
}

// Convert RGB888 to RGB565
static uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Convenience functions with buffer swapping for standalone usage
void DEMO_DisplayColorTest_AndSwap(uint16_t background_color)
{
    DEMO_DisplayColorTest(background_color);
    // Note: Buffer swap should be handled by main application loop
}

void DEMO_DrawRectangle_AndSwap(int x, int y, int w, int h, uint16_t color)
{
    DEMO_DrawRectangle(x, y, w, h, color);
    // Note: Buffer swap should be handled by main application loop
}

// Simple 5x7 bitmap font for digits and letters
static const uint8_t font_5x7[][7] = {
    // '0'
    {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F},
    // '1'  
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x1F},
    // '2'
    {0x1F, 0x01, 0x01, 0x1F, 0x10, 0x10, 0x1F},
    // '3'
    {0x1F, 0x01, 0x01, 0x1F, 0x01, 0x01, 0x1F},
    // '4'
    {0x11, 0x11, 0x11, 0x1F, 0x01, 0x01, 0x01},
    // '5'
    {0x1F, 0x10, 0x10, 0x1F, 0x01, 0x01, 0x1F},
    // '6'
    {0x1F, 0x10, 0x10, 0x1F, 0x11, 0x11, 0x1F},
    // '7'
    {0x1F, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
    // '8'
    {0x1F, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x1F},
    // '9'
    {0x1F, 0x11, 0x11, 0x1F, 0x01, 0x01, 0x1F},
    // 'F'
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    // 'P'
    {0x1F, 0x11, 0x11, 0x1F, 0x10, 0x10, 0x10},
    // 'S'
    {0x1F, 0x10, 0x10, 0x1F, 0x01, 0x01, 0x1F},
    // ':'
    {0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00},
    // 'O'
    {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F},
    // 'L'
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    // 'Y'
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04},
    // ' ' (space)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

// Character mapping
static char get_char_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == 'F') return 10;
    if (c == 'P') return 11; 
    if (c == 'S') return 12;
    if (c == ':') return 13;
    if (c == 'O') return 14;
    if (c == 'L') return 15;
    if (c == 'Y') return 16;
    return 17; // space
}

// Draw a single character
static void draw_char(int x, int y, char c, uint16_t color) {
    if (s_currentBackBuffer < 0 || s_currentBackBuffer >= 2) return;
    
    uint16_t *buffer = (uint16_t *)s_frameBuffer[s_currentBackBuffer];
    int char_idx = get_char_index(c);
    
    for (int row = 0; row < 7; row++) {
        uint8_t line = font_5x7[char_idx][row];
        for (int col = 0; col < 5; col++) {
            if (line & (0x10 >> col)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < LCD_WIDTH && py >= 0 && py < LCD_HEIGHT) {
                    buffer[py * LCD_WIDTH + px] = color;
                }
            }
        }
    }
}

// Draw text string
void DEMO_DrawText(int x, int y, const char* text, uint16_t color) {
    int current_x = x;
    for (int i = 0; text[i] != '\0'; i++) {
        draw_char(current_x, y, text[i], color);
        current_x += 6; // 5 pixels + 1 spacing
    }
}

// Draw number
void DEMO_DrawNumber(int x, int y, int number, uint16_t color) {
    char buffer[12]; // enough for 32-bit int
    
    // Simple integer to string conversion
    if (number == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
    } else {
        int i = 0;
        int temp = number;
        
        // Handle negative numbers
        if (number < 0) {
            temp = -number;
        }
        
        // Convert digits
        while (temp > 0) {
            buffer[i++] = '0' + (temp % 10);
            temp /= 10;
        }
        
        if (number < 0) {
            buffer[i++] = '-';
        }
        
        buffer[i] = '\0';
        
        // Reverse the string
        for (int j = 0; j < i / 2; j++) {
            char tmp = buffer[j];
            buffer[j] = buffer[i - 1 - j];
            buffer[i - 1 - j] = tmp;
        }
    }
    
    DEMO_DrawText(x, y, buffer, color);
}

/*******************************************************************************
 * Frame Buffer Accessor Functions for PXP Acceleration
 ******************************************************************************/

// Get pointer to current back buffer for PXP operations
uint8_t* DEMO_GetCurrentFrameBuffer(void)
{
    return s_frameBuffer[s_currentBackBuffer];
}

// Get current back buffer index
int DEMO_GetCurrentBackBufferIndex(void)
{
    return s_currentBackBuffer;
}

// Get frame buffer size
uint32_t DEMO_GetFrameBufferSize(void)
{
    return DEMO_FB_SIZE;
}

// Restore warning settings
#pragma GCC diagnostic pop
