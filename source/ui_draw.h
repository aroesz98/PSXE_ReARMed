/*
    Small drawing layer for the on-device UI (the game picker).

    Everything lands in the LCD frame buffer in RGB565, which is what the panel
    scans out - this has nothing to do with the emulator's VRAM format.
*/

#ifndef PSXE_UI_DRAW_H
#define PSXE_UI_DRAW_H

#include <stdint.h>

#include "ui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_WIDTH 480
#define UI_HEIGHT 272

#define UI_RGB(r, g, b) ((uint16_t)((((r) & 0xf8u) << 8) | (((g) & 0xfcu) << 3) | ((b) >> 3)))

/* Starts drawing into the current back buffer. */
void ui_begin(void);

/* Shows what was drawn and waits for the flip. */
void ui_present(void);

void ui_fill(int x, int y, int w, int h, uint16_t color);
void ui_fill_blend(int x, int y, int w, int h, uint16_t color, uint8_t alpha);
void ui_vgradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom);

/* Rounded rectangle with smooth corners. */
void ui_round_rect(int x, int y, int w, int h, int radius, uint16_t color);
void ui_round_rect_blend(int x, int y, int w, int h, int radius, uint16_t color, uint8_t alpha);

/* Width in pixels the string would take. */
int ui_text_width(const ui_font_t *font, const char *text);

/*
    Draws text with the pen at x and the baseline at y. Stops at max_width,
    ending the string with an ellipsis when it does not fit (max_width <= 0 means
    no limit). Returns the pen position after the text.
*/
int ui_text(const ui_font_t *font, int x, int y, int max_width, const char *text, uint16_t color);

#ifdef __cplusplus
}
#endif

#endif
