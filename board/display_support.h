/*
 * Copyright 2021, 2023 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LVGL_SUPPORT_H
#define LVGL_SUPPORT_H

#include <stdint.h>
#include <stdbool.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define DEMO_PANEL_RK043FN02H  0 /* RK043FN02H-CT */
#define DEMO_PANEL_RK043FN66HS 1 /* RK043FN66HS-CTG */

/* @TEST_ANCHOR */

#ifndef DEMO_PANEL
#define DEMO_PANEL DEMO_PANEL_RK043FN66HS
#endif

#define LCD_WIDTH  480
#define LCD_HEIGHT 272

/*******************************************************************************
 * API
 ******************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

void DEMO_InitLcd(void);
void lv_port_pre_init(void);
void lv_port_disp_init(void);
void lv_port_indev_init(void);
void DEMO_DisplayColorTest(uint16_t background_color);
void DEMO_DisplayColorTest_WithColor(uint16_t background_color);
void DEMO_DrawRectangle(int x, int y, int w, int h, uint16_t color);
void DEMO_Draw3DCube(int center_x, int center_y, int size, float rotation_x, float rotation_y, float rotation_z, uint16_t color);
void DEMO_Animate3DCube(void);
void DEMO_Animate3DSphere(void);
void DEMO_TestCenteredSphere(void);

// OBJ file rendering functions
void DEMO_DrawEyeball(int center_x, int center_y, int scale,
                      float rotation_x, float rotation_y, float rotation_z,
                      const char *obj_data, uint32_t obj_size);

// Generic 3D model rendering with solid color                      
int DEMO_Draw3DModel(int center_x, int center_y, float scale,
                     float rotation_x, float rotation_y, float rotation_z,
                     const char *obj_data, uint32_t obj_size, uint16_t color);

// Text rendering functions
void DEMO_DrawText(int x, int y, const char* text, uint16_t color);
void DEMO_DrawNumber(int x, int y, int number, uint16_t color);

// Buffer management
void DEMO_SwapBuffers(void);

// Frame buffer accessor functions for PXP acceleration
uint8_t* DEMO_GetCurrentFrameBuffer(void);
int DEMO_GetCurrentBackBufferIndex(void);
uint32_t DEMO_GetFrameBufferSize(void);

// Convenience functions with automatic buffer swapping for standalone usage
void DEMO_DisplayColorTest_AndSwap(uint16_t background_color);
void DEMO_DrawRectangle_AndSwap(int x, int y, int w, int h, uint16_t color);
void DEMO_Draw3DCube_AndSwap(int center_x, int center_y, int size, float rotation_x, float rotation_y, float rotation_z, uint16_t color);

// BMP texture functions
void DEMO_Draw3DCube_WithBMP(int center_x, int center_y, int size, 
                             float rotation_x, float rotation_y, float rotation_z, 
                             const uint8_t *bmp_data, uint32_t bmp_size);
void DEMO_ClearBMPCache(void);  // Clear cached BMP textures to free memory

// PXP Hardware Acceleration Functions
// These functions provide hardware-accelerated graphics operations using the i.MX RT PXP engine
// They automatically fall back to CPU implementation if PXP is unavailable

// Draw horizontal and vertical lines using PXP acceleration
bool PXP_DrawHorizontalLine(int x, int y, int width, uint16_t color);
bool PXP_DrawVerticalLine(int x, int y, int height, uint16_t color);

// Copy rectangular regions between buffers with optional scaling
bool PXP_CopyBuffer(void *src_buffer, int src_x, int src_y, int src_width, int src_height, 
                   int dest_x, int dest_y, int copy_width, int copy_height);
bool PXP_CopyBufferWithScale(void *src_buffer, int src_width, int src_height,
                            int dest_x, int dest_y, int dest_width, int dest_height);

// Rotate buffers (currently limited support - mainly for demonstration)
// Include the necessary PXP types for rotation function
#include "fsl_pxp.h"
bool PXP_RotateBuffer(void *src_buffer, int src_width, int src_height,
                     int dest_x, int dest_y, pxp_rotate_degree_t rotation);

// Example usage:
// Clear screen: DEMO_DisplayColorTest(0x0000); // Uses PXP acceleration internally
// Draw rectangle: DEMO_DrawRectangle(10, 10, 100, 50, 0xF800); // Uses PXP acceleration internally
// Draw line: PXP_DrawHorizontalLine(0, 50, 480, 0x07E0); // Green horizontal line
// Copy scaled: PXP_CopyBufferWithScale(my_buffer, 100, 100, 50, 50, 200, 200); // Scale and copy

// PXP Demo Functions (defined in pxp_demo.c)
void PXP_Demo_FillRectangles(void);
void PXP_Demo_DrawLines(void);
void PXP_Demo_TestPattern(void);
void PXP_Demo_BufferCopy(void);
void PXP_Demo_Complete(void);
void PXP_Demo_Benchmark(void);

// Smart PXP Functions (defined in pxp_smart.c)
void PXP_PerformanceDemo(void);
void PXP_ResetStats(void);
void PXP_PrintStats(void);

#if defined(__cplusplus)
}
#endif

#endif /*LVGL_SUPPORT_H */
