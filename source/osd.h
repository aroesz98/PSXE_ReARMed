/*
    On-screen status: a few short lines of text in the black bar left of the
    game picture (a 4:3 picture on the 480x272 panel leaves 59 pixels each
    side). The once a second status the build meant to be played used to print
    on the UART (speed, frame rate, temperature, the GPU board link) goes here
    instead.

    The lines are drawn into a small RGB565 buffer of their own, which the PXP
    puts over its output as the alpha surface when it scales a frame
    (screen.c) - so the status is part of every frame, whichever of the three
    LCD buffers it goes to.
*/
#ifndef PSXE_OSD_H
#define PSXE_OSD_H

#include <stdint.h>

#define OSD_W 56
#define OSD_H 272
#define OSD_LINES 13

/*
    While the emulator shows its picture the panel runs in BGR order (LCDIF_CTRL2, set in
    psxe_screen_init), so that the scaler can take a 15 bpp picture straight out of VRAM (screen.c).
    Whatever else is drawn for the LCD buffers in that time - the status here, the 24 bpp repack -
    swaps red and blue to match. The game picker before it draws RGB as usual.
*/
#ifndef PSXE_SCREEN_BGR
#define PSXE_SCREEN_BGR 1
#endif

/* an RGB565 colour the way the panel wants it while PSXE_SCREEN_BGR is in force */
#if PSXE_SCREEN_BGR
#define OSD_PANEL_COLOR(c) ((uint16_t)((((c) & 0x1fu) << 11) | ((c) & 0x07e0u) | (((c) >> 11) & 0x1fu)))
#else
#define OSD_PANEL_COLOR(c) ((uint16_t)(c))
#endif

/* Sets line `line` (0 at the top) to the formatted text in `color` (RGB565, see
   UI_RGB); only a change marks the status for redrawing. */
void osd_line(int line, uint16_t color, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Has any line changed since the buffer was last drawn? */
int osd_changed(void);

/* The status as OSD_W x OSD_H RGB565 pixels, redrawn first if a line changed. */
const uint16_t *osd_buffer(void);

#endif
