/*
 * Copyright 2020 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include "fsl_common.h"
#include "display_support.h"
#include "fsl_debug_console.h"
#include "fsl_pxp.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "fsl_cache.h"
#include "display.h"

/* Import functions and variables from display_support.c */
extern void DEMO_InitLcd(void);
extern void DEMO_SwapBuffers(void);

/* Provide BOARD_PrepareDisplayController as a wrapper */
static inline status_t BOARD_PrepareDisplayController(void)
{
    return kStatus_Success; /* Already handled by DEMO_InitLcd */
}

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define ENABLE_DISPLAY 1
#define ENABLE_PXP_INTERRUPT 1

/* Define if frame buffer has fixed address (not used in this example) */
#ifndef DEMO_BUFFER_FIXED_ADDRESS
#define DEMO_BUFFER_FIXED_ADDRESS 0
#endif

/*
 * In this project, the framebuffer pixel format is set to RGB565. The input YUV420
 * data is converted to RGB565, then sent to display controller.
 */
#ifdef DEMO_BUFFER_BYTE_PER_PIXEL
#undef DEMO_BUFFER_BYTE_PER_PIXEL
#endif
#define DEMO_BUFFER_BYTE_PER_PIXEL 2

#ifdef DEMO_BUFFER_PIXEL_FORMAT
#undef DEMO_BUFFER_PIXEL_FORMAT
#endif
#define DEMO_BUFFER_PIXEL_FORMAT kVIDEO_PixelFormatRGB565

/* Frame rate display settings */
#define FONT_WIDTH 8
#define FONT_HEIGHT 12
#define TEXT_COLOR 0xFFFF /* White in RGB565 */

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
#if ENABLE_DISPLAY
static int DEMO_InitPXP(void);
static int DEMO_InitFBDEV(void);
#endif

/*******************************************************************************
 * Variables
 ******************************************************************************/
#if ENABLE_DISPLAY

/* Use framebuffers from display_support.c instead of defining our own */
/* Frame buffer addresses are provided by display_support.c */
extern uint8_t *DEMO_GetCurrentFrameBuffer(void);

/* Provide compatibility addresses for old code */
const uint32_t s_frameBufferAddress[DEMO_BUFFER_COUNT] = {
    0x20240000, /* DEMO_BUFFER0_ADDR - will be set properly by display_support */
    0x20280000  /* DEMO_BUFFER1_ADDR - will be set properly by display_support */
};

/* PXP */
static pxp_output_buffer_config_t s_pxpOutputBufferConfig;
static pxp_ps_buffer_config_t s_pxpPsBufferConfig;
#if ENABLE_PXP_INTERRUPT
static SemaphoreHandle_t s_pxpCompleteSema;
#endif

#endif

/* Frame rate display variables */
static uint32_t s_currentFrameRate = 0;

/* Simple 8x12 bitmap font for digits 0-9, 'H', 'z', and space */
static const uint8_t s_font_8x12[][12] = {
    /* '0' */ {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C},
    /* '1' */ {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E},
    /* '2' */ {0x3C, 0x66, 0x06, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x60, 0x66, 0x7E},
    /* '3' */ {0x3C, 0x66, 0x06, 0x06, 0x06, 0x1C, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3C},
    /* '4' */ {0x0C, 0x1C, 0x3C, 0x6C, 0x6C, 0x6C, 0x7E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C},
    /* '5' */ {0x7E, 0x60, 0x60, 0x60, 0x7C, 0x06, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3C},
    /* '6' */ {0x3C, 0x66, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C},
    /* '7' */ {0x7E, 0x06, 0x06, 0x0C, 0x0C, 0x18, 0x18, 0x30, 0x30, 0x30, 0x30, 0x30},
    /* '8' */ {0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C},
    /* '9' */ {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x06, 0x66, 0x3C},
    /* 'H' */ {0x66, 0x66, 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66},
    /* 'z' */ {0x00, 0x00, 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00, 0x00, 0x00},
    /* ' ' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

/* Character indices */
#define CHAR_0 0
#define CHAR_1 1
#define CHAR_2 2
#define CHAR_3 3
#define CHAR_4 4
#define CHAR_5 5
#define CHAR_6 6
#define CHAR_7 7
#define CHAR_8 8
#define CHAR_9 9
#define CHAR_H 10
#define CHAR_Z 11
#define CHAR_SPACE 12

/*******************************************************************************
 * Code
 ******************************************************************************/

/* Helper function to get character index */
static uint8_t get_char_index(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else if (c == 'H')
        return CHAR_H;
    else if (c == 'z')
        return CHAR_Z;
    else
        return CHAR_SPACE;
}

/* Draw a character at specified position */
static void draw_char(uint16_t *framebuffer, uint16_t fb_width, uint16_t fb_height,
                      uint16_t x, uint16_t y, char c)
{
    uint8_t char_idx = get_char_index(c);

    for (int row = 0; row < FONT_HEIGHT; row++)
    {
        if (y + row >= fb_height)
            break;

        uint8_t line = s_font_8x12[char_idx][row];
        for (int col = 0; col < FONT_WIDTH; col++)
        {
            if (x + col >= fb_width)
                break;

            if (line & (0x80 >> col))
            {
                framebuffer[(y + row) * fb_width + (x + col)] = TEXT_COLOR;
            }
        }
    }
}

/* Draw frame rate text */
static void draw_frame_rate(uint16_t *framebuffer, uint16_t fb_width, uint16_t fb_height)
{
    char frameRateStr[8];
    sprintf(frameRateStr, "%dHz", (int)s_currentFrameRate);

    /* Position in top-right corner with some margin */
    uint16_t text_width = strlen(frameRateStr) * FONT_WIDTH;
    uint16_t start_x = fb_width - text_width - 10; /* 10 pixel margin from right */
    uint16_t start_y = 10;                         /* 10 pixel margin from top */

    /* Draw each character */
    for (int i = 0; frameRateStr[i] != '\0'; i++)
    {
        draw_char(framebuffer, fb_width, fb_height,
                  start_x + i * FONT_WIDTH, start_y, frameRateStr[i]);
    }
}

void DEMO_SetFrameRate(uint32_t frameRate)
{
    s_currentFrameRate = frameRate;
}

#if ENABLE_PXP_INTERRUPT
void PXP_IRQHandler(void)
{
    BaseType_t wake = pdFALSE;

    if (0U != (kPXP_CompleteFlag & PXP_GetStatusFlags(PXP)))
    {
        PXP_ClearStatusFlags(PXP, kPXP_CompleteFlag);

        (void)xSemaphoreGiveFromISR(s_pxpCompleteSema, &wake);

        portYIELD_FROM_ISR(wake);
    }
}
#endif

static int DEMO_InitPXP(void)
{
#if ENABLE_PXP_INTERRUPT
    /* Semaphore */
    s_pxpCompleteSema = xSemaphoreCreateBinary();

    if (NULL == s_pxpCompleteSema)
    {
        return -1;
    }
#endif

    /* Initialize variables. */
    memset(&s_pxpPsBufferConfig, 0, sizeof(s_pxpPsBufferConfig));
    memset(&s_pxpOutputBufferConfig, 0, sizeof(s_pxpOutputBufferConfig));

    s_pxpPsBufferConfig.pixelFormat = kPXP_PsPixelFormatYVU420;
    s_pxpPsBufferConfig.swapByte = false,

    s_pxpOutputBufferConfig.pixelFormat = kPXP_OutputPixelFormatRGB565;
    s_pxpOutputBufferConfig.interlacedMode = kPXP_OutputProgressive;
    s_pxpOutputBufferConfig.pitchBytes = (LCD_WIDTH * DEMO_BUFFER_BYTE_PER_PIXEL);

    /* Initialize hardware. */
    PXP_Init(PXP);

    PXP_SetProcessSurfaceBackGroundColor(PXP, 0U);

    /* Disable AS. */
    PXP_SetAlphaSurfacePosition(PXP, 0xFFFFU, 0xFFFFU, 0U, 0U);

    PXP_SetCsc1Mode(PXP, kPXP_Csc1YCbCr2RGB);
    PXP_EnableCsc1(PXP, true);

#if ENABLE_PXP_INTERRUPT
    NVIC_SetPriority(PXP_IRQn, 3);
    EnableIRQ(PXP_IRQn);

    PXP_EnableInterrupts(PXP, kPXP_CompleteInterruptEnable);
#endif

    return 0;
}

int DEMO_InitDisplay(void)
{
#if ENABLE_DISPLAY
    int ret;

    ret = DEMO_InitFBDEV();

    if (0 == ret)
    {
        ret = DEMO_InitPXP();
    }

    return ret;
#else
    return 0;
#endif
}