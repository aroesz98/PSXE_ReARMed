/*
    Drawing layer for the on-device UI. See ui_draw.h.

    The frame buffers live in the non-cacheable SDRAM region, so nothing here
    needs cache maintenance; they are also what eLCDIF scans out directly.
*/

#include "ui_draw.h"

#include <string.h>

#include "display_support.h"

static uint16_t *g_fb;
static int g_w = UI_WIDTH, g_h = UI_HEIGHT; /* the target's size, also its pitch in pixels */

void ui_begin(void)
{
    g_fb = (uint16_t *)DEMO_GetCurrentFrameBuffer();
    g_w = UI_WIDTH;
    g_h = UI_HEIGHT;
}

void ui_begin_buffer(uint16_t *buf, int w, int h)
{
    g_fb = buf;
    g_w = w;
    g_h = h;
}

void ui_present(void)
{
    DEMO_SwapBuffers();

    /*
        Wait for the panel to take the frame. Without this the next frame would
        be drawn into the buffer still on screen, because the swap is
        asynchronous and simply drops frames while one is pending - which is
        what made the picker flicker. The guard keeps a stalled display from
        hanging the boot.
    */
    for (uint32_t spin = 0; DEMO_IsFramePending() && (spin < 4000000u); spin++)
    {
        __asm volatile("nop");
    }
}

/* ------------------------------------------------------------------ pixels */

static inline void ui_clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0)
    {
        *w += *x;
        *x = 0;
    }

    if (*y < 0)
    {
        *h += *y;
        *y = 0;
    }

    if ((*x + *w) > g_w)
        *w = g_w - *x;

    if ((*y + *h) > g_h)
        *h = g_h - *y;
}

/* alpha is 0..255, where 255 is fully the new colour */
static inline uint16_t ui_mix(uint16_t dst, uint16_t src, uint32_t alpha)
{
    if (alpha >= 255u)
        return src;

    if (alpha == 0u)
        return dst;

    const uint32_t inv = 255u - alpha;

    const uint32_t sr = (src >> 11) & 0x1fu;
    const uint32_t sg = (src >> 5) & 0x3fu;
    const uint32_t sb = src & 0x1fu;

    const uint32_t dr = (dst >> 11) & 0x1fu;
    const uint32_t dg = (dst >> 5) & 0x3fu;
    const uint32_t db = dst & 0x1fu;

    const uint32_t r = (sr * alpha + dr * inv + 127u) / 255u;
    const uint32_t g = (sg * alpha + dg * inv + 127u) / 255u;
    const uint32_t b = (sb * alpha + db * inv + 127u) / 255u;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

void ui_fill(int x, int y, int w, int h, uint16_t color)
{
    ui_clip(&x, &y, &w, &h);

    if ((w <= 0) || (h <= 0) || !g_fb)
        return;

    for (int row = 0; row < h; row++)
    {
        uint16_t *d = g_fb + (uint32_t)(y + row) * g_w + x;

        for (int col = 0; col < w; col++)
            d[col] = color;
    }
}

void ui_fill_blend(int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    ui_clip(&x, &y, &w, &h);

    if ((w <= 0) || (h <= 0) || !g_fb)
        return;

    for (int row = 0; row < h; row++)
    {
        uint16_t *d = g_fb + (uint32_t)(y + row) * g_w + x;

        for (int col = 0; col < w; col++)
            d[col] = ui_mix(d[col], color, alpha);
    }
}

void ui_vgradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
    const int height = h;

    ui_clip(&x, &y, &w, &h);

    if ((w <= 0) || (h <= 0) || (height <= 0) || !g_fb)
        return;

    const int tr = (top >> 11) & 0x1f;
    const int tg = (top >> 5) & 0x3f;
    const int tb = top & 0x1f;
    const int dr = ((bottom >> 11) & 0x1f) - tr;
    const int dg = ((bottom >> 5) & 0x3f) - tg;
    const int db = (bottom & 0x1f) - tb;

    for (int row = 0; row < h; row++)
    {
        const int t = row;

        const uint16_t c = (uint16_t)((((tr + (dr * t) / height) & 0x1f) << 11) |
                                      (((tg + (dg * t) / height) & 0x3f) << 5) |
                                      ((tb + (db * t) / height) & 0x1f));

        uint16_t *d = g_fb + (uint32_t)(y + row) * g_w + x;

        for (int col = 0; col < w; col++)
            d[col] = c;
    }
}

/* ------------------------------------------------------------ rounded rects */

/*
    Coverage of one corner pixel: how much of it falls inside the quarter circle.
    Sampled on a 4x4 grid, which is smooth enough at these radii and needs no
    floating point.
*/
static uint32_t ui_corner_coverage(int dx, int dy, int radius)
{
    const int rr = radius * radius * 16;

    uint32_t inside = 0;

    for (int sy = 0; sy < 4; sy++)
    {
        for (int sx = 0; sx < 4; sx++)
        {
            const int px = dx * 4 + sx * 1 + 0; /* quarter pixel steps */
            const int py = dy * 4 + sy * 1 + 0;

            if ((px * px + py * py) <= rr)
                inside++;
        }
    }

    return (inside * 255u) / 16u;
}

static void ui_round_rect_impl(int x, int y, int w, int h, int radius, uint16_t color, uint32_t alpha)
{
    if ((w <= 0) || (h <= 0) || !g_fb)
        return;

    if (radius > (w / 2))
        radius = w / 2;

    if (radius > (h / 2))
        radius = h / 2;

    /* the straight middle band */
    if (alpha >= 255u)
        ui_fill(x, y + radius, w, h - 2 * radius, color);
    else
        ui_fill_blend(x, y + radius, w, h - 2 * radius, color, (uint8_t)alpha);

    for (int row = 0; row < radius; row++)
    {
        /* distance of this row from the corner centre */
        const int dy = radius - row - 1;

        int inset = 0;

        while ((inset < radius) && (ui_corner_coverage(radius - inset - 1, dy, radius) == 0u))
            inset++;

        const int top_y = y + row;
        const int bottom_y = y + h - 1 - row;

        /* the fully covered part of the row */
        const int solid_x = x + inset;
        const int solid_w = w - 2 * inset;

        if (solid_w > 0)
        {
            if (alpha >= 255u)
            {
                ui_fill(solid_x, top_y, solid_w, 1, color);
                ui_fill(solid_x, bottom_y, solid_w, 1, color);
            }
            else
            {
                ui_fill_blend(solid_x, top_y, solid_w, 1, color, (uint8_t)alpha);
                ui_fill_blend(solid_x, bottom_y, solid_w, 1, color, (uint8_t)alpha);
            }
        }

        /* and the partially covered pixels at both ends of both rows */
        for (int i = 0; i < inset; i++)
        {
            const uint32_t cov = ui_corner_coverage(radius - i - 1, dy, radius);

            if (!cov)
                continue;

            const uint32_t a = (cov * alpha) / 255u;

            const int lx = x + i;
            const int rx = x + w - 1 - i;

            if ((top_y >= 0) && (top_y < g_h))
            {
                uint16_t *d = g_fb + (uint32_t)top_y * g_w;

                if ((lx >= 0) && (lx < g_w))
                    d[lx] = ui_mix(d[lx], color, a);

                if ((rx >= 0) && (rx < g_w))
                    d[rx] = ui_mix(d[rx], color, a);
            }

            if ((bottom_y >= 0) && (bottom_y < g_h))
            {
                uint16_t *d = g_fb + (uint32_t)bottom_y * g_w;

                if ((lx >= 0) && (lx < g_w))
                    d[lx] = ui_mix(d[lx], color, a);

                if ((rx >= 0) && (rx < g_w))
                    d[rx] = ui_mix(d[rx], color, a);
            }
        }
    }
}

void ui_round_rect(int x, int y, int w, int h, int radius, uint16_t color)
{
    ui_round_rect_impl(x, y, w, h, radius, color, 255u);
}

void ui_round_rect_blend(int x, int y, int w, int h, int radius, uint16_t color, uint8_t alpha)
{
    ui_round_rect_impl(x, y, w, h, radius, color, alpha);
}

/* ------------------------------------------------------------------- text */

static const ui_glyph_t *ui_glyph(const ui_font_t *font, char c)
{
    uint8_t code = (uint8_t)c;

    if ((code < font->first) || (code >= (font->first + font->count)))
        code = '?';

    return &font->glyphs[code - font->first];
}

int ui_text_width(const ui_font_t *font, const char *text)
{
    int width = 0;

    for (const char *p = text; *p; p++)
        width += ui_glyph(font, *p)->advance;

    return width;
}

static void ui_glyph_draw(const ui_font_t *font, const ui_glyph_t *g, int pen_x, int baseline, uint16_t color)
{
    if (!g->width || !g->height)
        return;

    const uint8_t *bits = font->bitmap + g->offset;
    const int stride = (g->width + 1) / 2;

    for (int row = 0; row < g->height; row++)
    {
        const int py = baseline - font->ascent + g->top + row;

        if ((py < 0) || (py >= g_h))
            continue;

        uint16_t *d = g_fb + (uint32_t)py * g_w;
        const uint8_t *src = bits + (uint32_t)row * stride;

        for (int col = 0; col < g->width; col++)
        {
            const uint8_t byte = src[col >> 1];
            const uint32_t nib = (col & 1) ? (byte & 0x0fu) : (byte >> 4);

            if (!nib)
                continue;

            const int px = pen_x + g->left + col;

            if ((px < 0) || (px >= g_w))
                continue;

            /* 4 bit coverage to 0..255 */
            d[px] = ui_mix(d[px], color, (nib * 255u) / 15u);
        }
    }
}

int ui_text(const ui_font_t *font, int x, int y, int max_width, const char *text, uint16_t color)
{
    if (!g_fb || !text)
        return x;

    int pen = x;

    if (max_width > 0)
    {
        const int full = ui_text_width(font, text);

        if (full > max_width)
        {
            /* leave room for an ellipsis and cut the string there */
            const int dots = ui_glyph(font, '.')->advance * 3;
            const int limit = x + max_width - dots;

            for (const char *p = text; *p; p++)
            {
                const ui_glyph_t *g = ui_glyph(font, *p);

                if ((pen + g->advance) > limit)
                    break;

                ui_glyph_draw(font, g, pen, y, color);
                pen += g->advance;
            }

            for (int i = 0; i < 3; i++)
            {
                const ui_glyph_t *g = ui_glyph(font, '.');

                ui_glyph_draw(font, g, pen, y, color);
                pen += g->advance;
            }

            return pen;
        }
    }

    for (const char *p = text; *p; p++)
    {
        const ui_glyph_t *g = ui_glyph(font, *p);

        ui_glyph_draw(font, g, pen, y, color);
        pen += g->advance;
    }

    return pen;
}
