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

/* Sets line `line` (0 at the top) to the formatted text in `color` (RGB565, see
   UI_RGB); only a change marks the status for redrawing. */
void osd_line(int line, uint16_t color, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Has any line changed since the buffer was last drawn? */
int osd_changed(void);

/* The status as OSD_W x OSD_H RGB565 pixels, redrawn first if a line changed. */
const uint16_t *osd_buffer(void);

#endif
