#ifndef SCREEN_H
#define SCREEN_H

#include "../psx.h"
#include "common.h"

#include <string.h>
#include <stdint.h>

// MCU-specific includes for display
#include "fsl_elcdif.h"
#include "fsl_gpio.h"
#include "board.h"
#include "display_support.h"
#include "display.h"
#include "fsl_iomuxc.h"
#include "MIMXRT1052.h"

// PSX GPU constants
#include "../dev/gpu.h"

// Define screen pixel formats
#define SCREEN_PIXELFORMAT_BGR555 0
#define SCREEN_PIXELFORMAT_RGB24 1
#define SCREEN_PIXELFORMAT_RGB565 2

// Define input button mappings (replacing SDL keycodes)
#define SCREEN_BUTTON_CROSS 0x01
#define SCREEN_BUTTON_SQUARE 0x02
#define SCREEN_BUTTON_TRIANGLE 0x04
#define SCREEN_BUTTON_CIRCLE 0x08
#define SCREEN_BUTTON_START 0x10
#define SCREEN_BUTTON_SELECT 0x20
#define SCREEN_BUTTON_PAD_UP 0x40
#define SCREEN_BUTTON_PAD_DOWN 0x80
#define SCREEN_BUTTON_PAD_LEFT 0x100
#define SCREEN_BUTTON_PAD_RIGHT 0x200
#define SCREEN_BUTTON_L1 0x400
#define SCREEN_BUTTON_R1 0x800
#define SCREEN_BUTTON_L2 0x1000
#define SCREEN_BUTTON_R2 0x2000
#define SCREEN_BUTTON_L3 0x4000
#define SCREEN_BUTTON_R3 0x8000
#define SCREEN_BUTTON_ANALOG 0x10000

typedef struct
{
    // Display hardware handles
    void *framebuffer;
    void *backbuffer;

    // PSX emulator references
    psx_t *psx;
    psx_pad_t *pad;

    // Display parameters
    uint32_t saved_scale;
    uint32_t width, height, scale;
    uint32_t image_width, image_height;
    uint32_t image_xoff, image_yoff;
    uint32_t format;
    uint32_t texture_width, texture_height;

    // Display modes
    int32_t bilinear;
    int32_t fullscreen;
    int32_t vertical_mode;
    int32_t debug_mode;
    int32_t open;

    // Input state tracking
    uint32_t button_state;
    uint32_t prev_button_state;
} psxe_screen_t;

psxe_screen_t *psxe_screen_create(void);
void psxe_screen_init(psxe_screen_t *, psx_t *);
void psxe_screen_reload(psxe_screen_t *);
int32_t psxe_screen_is_open(psxe_screen_t *);
void psxe_screen_update(psxe_screen_t *);
void psxe_screen_destroy(psxe_screen_t *);
void psxe_screen_set_scale(psxe_screen_t *, uint32_t);
void psxe_screen_toggle_debug_mode(psxe_screen_t *);

// GPU event handlers
void psxe_gpu_dmode_event_cb(psx_gpu_t *);
void psxe_gpu_vblank_event_cb(psx_gpu_t *);

#endif