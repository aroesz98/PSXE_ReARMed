/* On-screen status, see osd.h */
#include "osd.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui_draw.h"

#define OSD_CHARS 16

static char s_text[OSD_LINES][OSD_CHARS];
static uint16_t s_color[OSD_LINES];
static int s_changed = 1;

/* read by the PXP: cleaned out of the D-cache with everything else before each job (screen.c) */
static uint16_t __attribute__((section(".bss.$BOARD_SDRAM"), aligned(32))) s_buf[OSD_W * OSD_H];

void osd_line(int line, uint16_t color, const char *fmt, ...)
{
    if ((line < 0) || (line >= OSD_LINES))
        return;

    char t[OSD_CHARS];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);

    if ((s_color[line] != color) || strcmp(s_text[line], t))
    {
        memcpy(s_text[line], t, sizeof(t));
        s_color[line] = color;
        s_changed = 1;
    }
}

int osd_changed(void)
{
    return s_changed;
}

const uint16_t *osd_buffer(void)
{
    if (!s_changed)
        return s_buf;

    s_changed = 0;

    ui_begin_buffer(s_buf, OSD_W, OSD_H);
    ui_fill(0, 0, OSD_W, OSD_H, UI_RGB(0, 0, 0));

    for (int i = 0; i < OSD_LINES; i++)
        if (s_text[i][0])
            ui_text(&ui_font_small, 3, 6 + ui_font_small.ascent + (i * ui_font_small.line_height), OSD_W - 4,
                    s_text[i], s_color[i]);

    return s_buf;
}
