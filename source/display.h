/*
 * Copyright 2020 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include "display_support.h"

#if defined(__cplusplus)
extern "C"
{
#endif /* __cplusplus */

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/* Use definitions from display_support.h for compatibility */
#define DEMO_BUFFER_COUNT 2
#define DEMO_BUFFER_WIDTH LCD_WIDTH
#define DEMO_BUFFER_HEIGHT LCD_HEIGHT
#define DEMO_BUFFER_START_X 0
#define DEMO_BUFFER_START_Y 0
#define DEMO_BUFFER_BYTE_PER_PIXEL 2
#define DEMO_BUFFER_PIXEL_FORMAT kVIDEO_PixelFormatRGB565

    /* Frame buffer addresses - will be provided by display_support */
    extern const uint32_t s_frameBufferAddress[DEMO_BUFFER_COUNT];

    /*******************************************************************************
     * API
     ******************************************************************************/

    /*
     * Return value:
     *  - 0: Initialize success.
     *  - Not 0: Failed.
     */
    int DEMO_InitDisplay(void);

    void DEMO_DisplayFrame(uint16_t width,
                           uint16_t height,
                           const uint8_t *Y,
                           const uint8_t *U,
                           const uint8_t *V,
                           uint32_t Y_Stride,
                           uint32_t UV_Stride);

    void DEMO_SetFrameRate(uint32_t frameRate);

#if defined(__cplusplus)
}
#endif /* __cplusplus */

#endif
