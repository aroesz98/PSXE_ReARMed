#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gpu.h"
#include "log.h"
#include "fixed_math.h"
#include "fsl_debug_console.h"
#include "../prof.h"
#include "../gpu_switch.h"


#if PSXE_GPU_REMOTE
#include "../link/gpu_remote.h"
#endif

/*
    Where things go and what they are made of is the platform's business: the
    same file is the rasterizer of the RT1050 build and of the GPU board. The
    defaults are the RT1050's; a standalone build defines them first.
*/
#ifdef PSX_GPU_STANDALONE
#include "psx_gpu_platform.h"
#endif

/*
    VRAM's pixel format, and who draws.

    By default this file is the whole GPU: VRAM holds the PSX's own BGR555 and
    every primitive is rasterized here. A platform whose drawing is done by
    hardware defines PSX_GPU_EXTERNAL_RASTER and the two conversions below - see
    h7s7_psxgpu/src/nema_raster.c, where the NeoChrom draws into VRAM kept in
    RGBA5551, the only 16 bit format that GPU2D writes. What the game uploads
    and reads back is converted at those two points, nowhere else.
*/
#ifndef PSX_PIX_FROM_PSX
#define PSX_PIX_FROM_PSX(p) (p)
#define PSX_PIX_TO_PSX(p) (p)
#endif

#if PSX_GPU_EXTERNAL_RASTER
/*
    Does this upload carry colour or palette indices?

    VRAM holds both: a frame buffer, whose pixels the hardware draws and shows,
    and texture pages, which for 4 and 8 bit textures are indices packed four or
    two to a halfword. When the drawing is done by hardware that writes its own
    pixel format, the frame buffer has to be in that format and the indices must
    not be touched - and what a transfer carries is told by where it lands: a
    frame buffer is what the display window and the drawing area point at, and
    textures live everywhere else.
*/
static inline void gpu_set_rect(uint16_t *r, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    r[0] = (uint16_t)((x0 < 1024u) ? x0 : 1024u);
    r[1] = (uint16_t)((y0 < 512u) ? y0 : 512u);
    r[2] = (uint16_t)((x1 < 1024u) ? x1 : 1024u);
    r[3] = (uint16_t)((y1 < 512u) ? y1 : 512u);

    if ((r[2] <= r[0]) || (r[3] <= r[1]))
        r[2] = r[0];
}

/*
    Judged pixel by pixel, not per transfer: a transfer that straddles a
    frame buffer and a texture page - the other board restoring VRAM in strips
    of whole rows does exactly that - must convert the one and leave the other
    alone, or every palette index in those rows is scrambled for good.
    A 24 bit picture on screen (video) is packed bytes, never converted: the
    presenter repacks it itself.
*/
static void gpu_img_rects(psx_gpu_t *gpu)
{
    static const uint32_t hres[4] = {256, 320, 512, 640};

    if (gpu->display_mode & 0x10u)
    {
        gpu->img_rect[0][2] = gpu->img_rect[0][0];
        gpu->img_rect[1][2] = gpu->img_rect[1][0];

        return;
    }

    const uint32_t dw = (gpu->display_mode & 0x40u) ? 368u : hres[gpu->display_mode & 3u];
    const uint32_t dh = ((gpu->display_mode & 0x24u) == 0x24u) ? 480u : 240u;

    gpu_set_rect(gpu->img_rect[0], gpu->disp_x, gpu->disp_y, gpu->disp_x + dw, gpu->disp_y + dh);
    gpu_set_rect(gpu->img_rect[1], gpu->draw_x1, gpu->draw_y1, gpu->draw_x2 + 1u, gpu->draw_y2 + 1u);
}

static inline int gpu_pix_is_image(const psx_gpu_t *gpu, uint32_t x, uint32_t y)
{
    for (int i = 0; i < 2; i++)
    {
        const uint16_t *r = gpu->img_rect[i];

        if ((x >= r[0]) && (x < r[2]) && (y >= r[1]) && (y < r[3]))
            return 1;
    }

    return 0;
}

/* n pixels of one row of an upload into VRAM at (x, y), no wrap: raw, and the
   part of them that lies in a frame buffer converted */
static void gpu_upload_row(psx_gpu_t *gpu, uint16_t *dst, const uint16_t *src, uint32_t x, uint32_t y, uint32_t n)
{
    memcpy(dst, src, n * 2u);

    for (int i = 0; i < 2; i++)
    {
        const uint16_t *r = gpu->img_rect[i];

        if ((y < r[1]) || (y >= r[3]) || (r[2] <= r[0]))
            continue;

        const uint32_t a = (x > r[0]) ? x : r[0];
        const uint32_t b = ((x + n) < r[2]) ? (x + n) : r[2];

        for (uint32_t k = a; k < b; k++)
            dst[k - x] = PSX_PIX_FROM_PSX(src[k - x]);
    }
}

int psx_raster_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data);
int psx_raster_rect(psx_gpu_t *gpu, rect_data_t data);
int psx_raster_line(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, uint16_t color, int transp);
int psx_raster_fill(psx_gpu_t *gpu, int x, int y, int w, int h, uint32_t color);
void psx_raster_sync(void);
#define PSX_RASTER_SYNC() psx_raster_sync()
#define PSX_UPLOAD_PIX(gpu, x, y, v)     (gpu_pix_is_image((gpu), (x), (y)) ? PSX_PIX_FROM_PSX((uint16_t)(v)) : (uint16_t)(v))
#define PSX_READ_PIX(gpu, x, y, v)     (gpu_pix_is_image((gpu), (x), (y)) ? PSX_PIX_TO_PSX((uint16_t)(v)) : (uint16_t)(v))
#else
#define PSX_RASTER_SYNC() do {} while (0)
#define PSX_UPLOAD_PIX(gpu, x, y, v) ((uint16_t)(v))
#define PSX_READ_PIX(gpu, x, y, v) ((uint16_t)(v))
#endif

/* the CPU is about to read rows of VRAM the drawing hardware may have written:
   its data cache must not answer with what was there before */
#ifndef PSX_VRAM_CPU_READ
#define PSX_VRAM_CPU_READ(gpu, y, rows) do {} while (0)
#endif

/* Hot GPU code runs from OCRAM instead of XIP flash (ITCM is reserved for
   the CPU interpreter). */
#define GPU_CYCLES_PER_HDRAW_NTSC 2560.0f
#define GPU_CYCLES_PER_SCANL_NTSC 3413.0f
#define GPU_SCANS_PER_VDRAW_NTSC 240
#define GPU_SCANS_PER_FRAME_NTSC 263
#define GPU_CYCLES_PER_SCANL_PAL 3406.0f
#define GPU_SCANS_PER_FRAME_PAL 314

/* 16.16 fixed point versions of the scanline constants and of the CPU -> GPU
   clock ratio: the periodic GPU update runs on integers only */
#define GPU_FP_HDRAW_NTSC ((uint32_t)(GPU_CYCLES_PER_HDRAW_NTSC * 65536.0f))
#define GPU_FP_SCANL_NTSC ((uint32_t)(GPU_CYCLES_PER_SCANL_NTSC * 65536.0f))
/* GPU cycles per CPU cycle, 16.16 fixed point.

   Built from the hardware frequencies directly: the GPU dot clock runs at
   53.693175 MHz and the CPU at 33.868800 MHz, so the GPU advances 1.58537
   cycles per CPU cycle. The scaled PSX_GPU_CLOCK_FREQ_NTSC / PSX_CPU_FREQ pair
   that used to build this ratio was off by a factor of ten, which made every
   emulated vblank arrive ten times too early: the game then redrew the screen
   ten times per frame worth of CPU work and the rasteriser ate most of the
   core. */
#define GPU_HW_CLOCK_NTSC 53.693175f
#define GPU_HW_CLOCK_PAL 53.203425f
#define CPU_HW_CLOCK 33.868800f

#define GPU_FP_RATIO ((uint32_t)((GPU_HW_CLOCK_NTSC / CPU_HW_CLOCK) * 65536.0f))

#ifndef PSX_GPU_HOT
#define PSX_GPU_HOT __attribute__((section(".ramfunc.$SRAM_OC")))
#endif

/* what the periodic timing update needs: zero wait states */
/* the span level of the rasterizer: the innermost code, where a platform with a
   small zero wait state memory (ITCM) puts it; elsewhere the same as PSX_GPU_HOT */
#ifndef PSX_GPU_RAS
#define PSX_GPU_RAS PSX_GPU_HOT
#endif
#ifndef PSX_GPU_ITC
#define PSX_GPU_ITC __attribute__((section(".ramfunc.$SRAM_ITC")))
#endif

/* state touched by every command: DTCM, out of the way of the VRAM traffic */
#ifndef PSX_GPU_DTCM_BSS
#define PSX_GPU_DTCM_BSS __attribute__((section(".bss.$SRAM_DTC")))
#endif

#ifndef psx_gpu_alloc
#define psx_gpu_alloc(size) malloc(size)
#define psx_gpu_free(p) free(p)
#endif
#ifndef psx_gpu_alloc_empty
/* the buffer shown while the display is off; a platform without one returns NULL */
#define psx_gpu_alloc_empty(size) psx_gpu_alloc(size)
#endif

#define SE10(v) ((int16_t)((v) << 5) >> 5)
#define swap_coord(a, b)    \
    do                      \
    {                       \
        int32_t temp = (a); \
        (a) = (b);          \
        (b) = temp;         \
    } while (0)

int g_psx_gpu_dither_kernel[] = {
    -4,
    +0,
    -3,
    +1,
    +2,
    -2,
    +3,
    -1,
    -3,
    +1,
    -4,
    +0,
    +3,
    -1,
    +2,
    -2,
};

/*
    A PSX VRAM pixel: mask bit 15, blue 14:10, green 9:5, red 4:0.

    Everything in VRAM is kept in this native layout - that is how textures and
    directly uploaded images arrive from the game - and the display path converts
    the visible area to RGB565 once per frame.
*/
static inline uint16_t rgb888_to_bgr555(uint32_t color)
{
    return PSX_PIX_FROM_PSX((uint16_t)(((color & 0x0000f8u) >> 3) | ((color & 0x00f800u) >> 6) |
                                       ((color & 0xf80000u) >> 9)));
}

PSX_GPU_HOT int min3(int a, int b, int c)
{
    int m = (a <= b) ? a : b;

    return (m <= c) ? m : c;
}

PSX_GPU_HOT int max3(int a, int b, int c)
{
    int m = (a > b) ? a : b;

    return (m > c) ? m : c;
}

/* The GPU state is touched by every device update round and by every GP0
   command, so it belongs in DTCM - out of the D-cache the rasterizer keeps
   thrashing with VRAM traffic. */
static psx_gpu_t PSX_GPU_DTCM_BSS __attribute__((aligned(8))) g_gpu_instance;

psx_gpu_t *psx_gpu_create(void)
{
    memset(&g_gpu_instance, 0, sizeof(g_gpu_instance));

    return &g_gpu_instance;
}

static PSX_GPU_HOT void psx_gpu_update_cmd_impl(psx_gpu_t *gpu);

/*
    vram_dirty: "the picture on the panel is out of date".

    Presenting a frame means reading the whole display window out of SDRAM,
    repacking it and scaling it - about 2 M core cycles - so it should happen
    when the picture changed and only then. "Something was written to the GPU"
    is far too eager a test for that: games draw the next frame into a buffer
    that is not on screen and then move the display window onto it, so between
    two flips every write goes to VRAM that is not visible. Counting those made
    the panel get the same picture two to four times over, and during full
    motion video, where a frame is uploaded strip by strip over several vertical
    blanks, it was most of the presenting work.

    So the flag is raised when a write lands inside the display window, and when
    the window itself moves or changes shape (see GP1 below).

    The top and bottom GPU_DIRTY_MARGIN lines of the window do not count: they
    are overscan on a television, and FF7's video player keeps its second frame
    buffer eight lines into the first one's window, which would otherwise make
    every strip it uploads look visible.
*/
#define GPU_DIRTY_MARGIN 8u

/* the display window in VRAM coordinates: [x0, x1) x [y0, y1) */
static inline void gpu_window(const psx_gpu_t *gpu, uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1)
{
    static const uint16_t hres[4] = {256, 320, 512, 640};

    const uint32_t mode = gpu->display_mode;

    uint32_t w = (mode & 0x40u) ? 368u : hres[mode & 3u];

    if (mode & 0x10u)
        w = (w * 3u + 1u) / 2u; /* 24 bpp: three bytes per pixel in 16 bit cells */

    const uint32_t h = ((mode & 0x24u) == 0x24u) ? 480u : 240u;

    *x0 = gpu->disp_x;
    *x1 = gpu->disp_x + w;
    *y0 = gpu->disp_y;
    *y1 = gpu->disp_y + h;
}

/* Is any of [x0, x1) x [y0, y1), in VRAM coordinates, inside the display window
   less `margin` rows at its top and bottom? */
static inline int gpu_rect_in_window(const psx_gpu_t *gpu, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                                     uint32_t margin)
{
    /* empty, or wrapping around VRAM: not worth working out */
    if ((x1 > 1024u) || (y1 > 512u) || (x1 <= x0) || (y1 <= y0))
        return 1;

    uint32_t wx0, wy0, wx1, wy1;

    gpu_window(gpu, &wx0, &wy0, &wx1, &wy1);

    wy0 += margin;
    wy1 -= margin;

    return (x0 < wx1) && (x1 > wx0) && (y0 < wy1) && (y1 > wy0);
}

/* Is all of [x0, x1) x [y0, y1) inside the display window? (Not when either
   wraps around VRAM - that is only ever "maybe".) */
static inline int gpu_rect_inside_window(const psx_gpu_t *gpu, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t wx0, wy0, wx1, wy1;

    gpu_window(gpu, &wx0, &wy0, &wx1, &wy1);

    return (x0 < x1) && (y0 < y1) && (x0 >= wx0) && (x1 <= wx1) && (y0 >= wy0) && (y1 <= wy1) &&
           (wx1 <= 1024u) && (wy1 <= 512u);
}

/* ... on screen, as far as a new frame is concerned? */
static inline int gpu_rect_visible(const psx_gpu_t *gpu, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    return gpu_rect_in_window(gpu, x0, y0, x1, y1, GPU_DIRTY_MARGIN);
}

/*
    Which rows went out of date is kept as well (dirty_y0 .. dirty_y1). A game
    can draw into the buffer that is on screen: FF7 renders a battle into the
    back buffer at 15 frames a second and flips, but draws its menus and gauges
    over the visible one twice in between. Those are changes the panel has to
    show, only there is no need to read the whole picture out of SDRAM again for
    a few dozen rows of menu: the frontend repacks just the rows named here when
    the display window is still where it was.

    The rows are those of the whole window: a write into the overscan margin is
    no reason for a new frame, but when the next one is made those rows have to
    be read again like any other, or the copy the frontend keeps goes stale.
*/
static inline void gpu_dirty_rows(psx_gpu_t *gpu, uint32_t y0, uint32_t y1)
{
    if (y1 > 512u)
        y1 = 512u;

    if (y0 < gpu->dirty_y0)
        gpu->dirty_y0 = (uint16_t)y0;

    if (y1 > gpu->dirty_y1)
        gpu->dirty_y1 = (uint16_t)y1;
}

/* the whole picture: the window moved, changed shape, or nobody knows */
static inline void gpu_dirty_all(psx_gpu_t *gpu)
{
    gpu->vram_dirty = 1;
    gpu->dirty_y0 = 0;
    gpu->dirty_y1 = 512;
}

/* [x0, x1) x [y0, y1) was written */
static inline void gpu_touch(psx_gpu_t *gpu, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    /* called with every word of an upload: one test while nothing new comes of it */
    if (gpu->vram_dirty && (y0 >= gpu->dirty_y0) && (y1 <= gpu->dirty_y1))
        return;

    if (!gpu_rect_in_window(gpu, x0, y0, x1, y1, 0))
        return;

    const int visible = gpu_rect_visible(gpu, x0, y0, x1, y1);

    if (visible && !gpu->vram_dirty)
        PROF_INC(dirty_draw);

    if ((y1 > 512u) || (y1 <= y0))
    {
        gpu_dirty_all(gpu); /* wraps around VRAM */

        return;
    }

    if (visible)
        gpu->vram_dirty = 1;

    gpu_dirty_rows(gpu, y0, y1);
}

/*
    Leaving out the field nobody sees.

    In 480 line field drawing (gpu_update_field) a game draws every frame into
    one field of its frame buffer, the two fields in turn. A frontend whose
    panel has fewer lines than that shows one field of the picture - the rows
    from disp_y on in steps of two - so every other frame the game draws goes
    into rows that never reach the screen: Tekken 3 spent half of its
    rasterizer time there. While the frontend says it shows one field
    (psx_gpu_set_one_field), those frames are not drawn: polygons, rectangles
    and lines while the drawing area lies inside the display window, and fills
    that do.

    The rows left out are then out of date, which matters only if the game
    reads them back - copies them, reads them to the CPU, or textures from
    them - and they stay so when the game stops field drawing (E1.10) but not
    the 480 line display. So for as long as the display is 480 line
    interlaced, reads of the display window are watched, and the first one
    stops the leaving out, until the display is something else (a game
    redraws its picture then anyway).
*/
static inline void gpu_update_hidden(psx_gpu_t *gpu)
{
    gpu->field_guard = ((gpu->display_mode & 0x24u) == 0x24u) && gpu->one_field && !gpu->field_reads;

    /* field drawing, and the field on screen is the one shown: the game draws into the other */
    gpu->field_hidden = gpu->field_guard && (gpu->skip_rows >= 0) &&
                        ((uint32_t)gpu->skip_rows == (gpu->disp_y & 1u));

    gpu->skip_prims = gpu->field_hidden && gpu->draw_in_window;
}

/* [x0, x1) x [y0, y1) of VRAM is about to be read (as texture, palette, copy
   source or by the CPU): if that is the display window, stop leaving fields out */
static void gpu_field_read(psx_gpu_t *gpu, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (!gpu->field_guard || (x1 <= x0) || (y1 <= y0))
        return;

    /* past the right or the bottom edge of VRAM a read goes on at the other
       side: up to two ranges each way */
    uint32_t xs[2][2], ys[2][2];
    int nx = 1, ny = 1;

    if ((x1 - x0) >= 1024u)
    {
        x0 = 0;
        x1 = 1024u;
    }

    x0 &= 0x3ffu;
    x1 = x0 + ((x1 - x0) & 0x7ffu);

    xs[0][0] = x0;
    xs[0][1] = (x1 > 1024u) ? 1024u : x1;

    if (x1 > 1024u)
    {
        xs[1][0] = 0;
        xs[1][1] = x1 - 1024u;
        nx = 2;
    }

    if ((y1 - y0) >= 512u)
    {
        y0 = 0;
        y1 = 512u;
    }

    y0 &= 0x1ffu;
    y1 = y0 + ((y1 - y0) & 0x3ffu);

    ys[0][0] = y0;
    ys[0][1] = (y1 > 512u) ? 512u : y1;

    if (y1 > 512u)
    {
        ys[1][0] = 0;
        ys[1][1] = y1 - 512u;
        ny = 2;
    }

    for (int i = 0; i < nx; i++)
        for (int j = 0; j < ny; j++)
            if (gpu_rect_in_window(gpu, xs[i][0], ys[j][0], xs[i][1], ys[j][1], 0))
            {
                gpu->field_reads = 1;
                gpu_update_hidden(gpu);

                return;
            }
}

/* the same for a textured primitive: texels u0..u1, v0..v1 of the page at
   (tpx, tpy) of the given depth, and its palette */
static inline void gpu_field_read_tex(psx_gpu_t *gpu, uint32_t tpx, uint32_t tpy, uint32_t depth, uint32_t clut,
                                      uint32_t u0, uint32_t u1, uint32_t v0, uint32_t v1)
{
    /* which halfword of the page texel u is in: u >> 2, 1, 0 - the reserved
       depth 3 is read as 4, 8 or 15 bit depending on the loop, so all of them */
    const uint32_t shift_lo = (depth == 0) ? 2u : ((depth == 1) ? 1u : ((depth == 2) ? 0u : 2u));
    const uint32_t shift_hi = (depth == 0) ? 2u : ((depth == 1) ? 1u : 0u);

    if (gpu->texw_mx || gpu->texw_my)
    {
        u0 = 0;
        u1 = 255;
        v0 = 0;
        v1 = 255;
    }

    gpu_field_read(gpu, tpx + (u0 >> shift_lo), tpy + v0, tpx + (u1 >> shift_hi) + 1u, tpy + v1 + 1u);

    if (depth != 2)
    {
        const uint32_t cx = (clut & 0x3fu) << 4;
        const uint32_t cy = (clut >> 6) & 0x1ffu;

        gpu_field_read(gpu, cx, cy, cx + ((depth == 0) ? 16u : 256u), cy + 1u);
    }
}

void psx_gpu_set_one_field(psx_gpu_t *gpu, int32_t on)
{
    gpu->one_field = on ? 1 : 0;

    gpu_update_hidden(gpu);
}

/* Polygons, lines and rectangles are clipped to the drawing area, so whether
   they can show is a property of that area and of the display window - worked
   out when either changes instead of once per primitive. The rows of the window
   are kept too: a drawing area that spans both of a game's frame buffers is
   "visible", the primitives that go into the hidden one are not. */
static inline void gpu_update_draw_visible(psx_gpu_t *gpu)
{
    /* the drawing area as far as this board is concerned: its share of the game's */
    if (gpu->band_share >= 256)
    {
        gpu->draw_ry1 = (int32_t)gpu->draw_y1;
        gpu->draw_ry2 = (int32_t)gpu->draw_y2;
    }
    else
    {
        const int32_t h = (int32_t)gpu->draw_y2 - (int32_t)gpu->draw_y1 + 1;
        const int32_t cut = (int32_t)gpu->draw_y1 + ((h * gpu->band_share) >> 8);

        if (gpu->band_top)
        {
            gpu->draw_ry1 = (int32_t)gpu->draw_y1;
            gpu->draw_ry2 = cut - 1;
        }
        else
        {
            gpu->draw_ry1 = cut;
            gpu->draw_ry2 = (int32_t)gpu->draw_y2;
        }
    }

    gpu->draw_visible = (gpu->draw_ry1 <= gpu->draw_ry2) &&
                        gpu_rect_visible(gpu, gpu->draw_x1, (uint32_t)gpu->draw_ry1, gpu->draw_x2 + 1u,
                                         (uint32_t)gpu->draw_ry2 + 1u);

    const uint32_t h = ((gpu->display_mode & 0x24u) == 0x24u) ? 480u : 240u;

    gpu->vis_y0 = (uint16_t)gpu->disp_y;
    gpu->vis_y1 = (uint16_t)(gpu->disp_y + h);

    gpu->draw_in_window = (gpu->draw_ry1 <= gpu->draw_ry2) &&
                          gpu_rect_inside_window(gpu, gpu->draw_x1, (uint32_t)gpu->draw_ry1, gpu->draw_x2 + 1u,
                                                 (uint32_t)gpu->draw_ry2 + 1u);

    gpu_update_hidden(gpu);
}

/*
    Interlaced 480 line output shows the even and the odd rows of the frame
    buffer in turn, one field per vertical blank. A game that makes a whole
    frame per field keeps a single frame buffer and clears GP0(E1).10 ("drawing
    to the display area prohibited"): the GPU then leaves the rows of the field
    on screen alone and draws only the others, which go on screen at the next
    blank. Polygons, rectangles, lines and fills skip those rows, in all of
    VRAM; uploads and copies do not. The field changes at the start of the
    blank, before the game is told of it and draws the next frame. (As Mednafen
    and DuckStation have it.)

    Tekken 3 draws that way at 368x480, so this halves its rasterizer work.
*/
static inline void gpu_update_field(psx_gpu_t *gpu)
{
    if (((gpu->display_mode & 0x24u) == 0x24u) && !(gpu->gpustat & 0x400u))
        gpu->skip_rows = (int32_t)((gpu->disp_y + (uint32_t)gpu->field) & 1u);
    else
        gpu->skip_rows = -1;

    /* no 480 line picture any more: fields may be left out again (gpu_update_hidden) */
    if ((gpu->display_mode & 0x24u) != 0x24u)
        gpu->field_reads = 0;

    gpu_update_hidden(gpu);
}

/*
    The frontend's scaler may still be reading the picture it was handed at the
    last blank straight out of VRAM (screen.c gives a 15 bpp picture to the PXP
    without a copy): rows [y0, y1) of it, in g_psx_vram_reader as y0 | y1 << 16,
    0 while nothing reads. A game that puts its other buffer on screen right
    after the blank and clears the one that was shown - Atlantis - cleared it
    under the scaler's feet now and then: the frame came out with a band of the
    clear colour from where the scaler had got to down to the bottom. So what is
    about to write those rows waits for the scaler first. That is rare: mostly
    it is done long before.
*/
volatile uint32_t g_psx_vram_reader;

/* the frontend's wait (screen.c); without one there is nobody to wait for */
void __attribute__((weak)) psx_platform_vram_reader_wait(void)
{
    g_psx_vram_reader = 0u;
}

static inline void gpu_reader_guard(uint32_t y0, uint32_t y1)
{
    const uint32_t r = g_psx_vram_reader;

    if (r && (y0 < (r >> 16)) && (y1 > (r & 0xffffu)))
        psx_platform_vram_reader_wait();
}

/* rows y .. y + h - 1 of a rectangle that wraps around the bottom of VRAM */
static inline void gpu_reader_guard_rows(uint32_t y, uint32_t h)
{
    if (!g_psx_vram_reader)
        return;

    if ((y + h) > 512u)
        gpu_reader_guard(0u, 512u);
    else
        gpu_reader_guard(y, y + h);
}

/* A polygon or a rectangle is about to draw into rows [y0, y1) of VRAM, which
   are inside the drawing area already */
static inline void gpu_prim_rows(psx_gpu_t *gpu, int32_t y0, int32_t y1)
{
    gpu_reader_guard((uint32_t)y0, (uint32_t)y1);

    /* vis_y0 .. vis_y1 are the rows of the window, margin included */
    if ((y0 >= (int32_t)gpu->vis_y1) || (y1 <= (int32_t)gpu->vis_y0))
        return;

    gpu_dirty_rows(gpu, (uint32_t)y0, (uint32_t)y1);

    if (!gpu->vram_dirty && gpu->draw_visible && (y0 < ((int32_t)gpu->vis_y1 - (int32_t)GPU_DIRTY_MARGIN)) &&
        (y1 > ((int32_t)gpu->vis_y0 + (int32_t)GPU_DIRTY_MARGIN)))
    {
        gpu->vram_dirty = 1;
        PROF_INC(dirty_draw);
    }
}

/* A GP0 word arrived for the command in buf[0]: note what it can change.
   Called for every word, so the common case has to be the first test. */
static inline void gpu_note_cmd(psx_gpu_t *gpu)
{
    const uint32_t cmd = gpu->buf[0] >> 24;

    /* polygons and rectangles say which rows they draw when they are drawn
       (gpu_prim_rows); lines are rare enough to count as the whole picture */
    if ((cmd >= 0x20u) && (cmd < 0x80u))
    {
        if ((cmd >= 0x40u) && (cmd < 0x60u) && gpu->draw_visible && !gpu->skip_prims)
        {
            if (!gpu->vram_dirty)
                PROF_INC(dirty_draw);

            gpu_dirty_all(gpu);
        }

        return;
    }

    /* fill and VRAM to VRAM copy ignore the drawing area; their rectangle is in
       the arguments, which are complete when the last one has arrived */
    if ((gpu->state != GPU_STATE_RECV_ARGS) || gpu->cmd_args_remaining)
        return;

    if (cmd == 0x02u)
    {
        const uint32_t x = gpu->buf[1] & 0x3f0u;
        const uint32_t y = (gpu->buf[1] >> 16) & 0x1ffu;
        const uint32_t w = ((gpu->buf[2] & 0x3ffu) + 0x0fu) & ~0x0fu;
        const uint32_t h = (gpu->buf[2] >> 16) & 0x1ffu;

        if (w && h)
            gpu_touch(gpu, x, y, x + w, y + h);
    }
    else if (cmd == 0x80u)
    {
        const uint32_t x = gpu->buf[2] & 0xffffu;
        const uint32_t y = gpu->buf[2] >> 16;
        const uint32_t w = gpu->buf[3] & 0xffffu;
        const uint32_t h = gpu->buf[3] >> 16;

        if (w && h)
            gpu_touch(gpu, x, y, x + w, y + h);
    }
}

void psx_gpu_init(psx_gpu_t *gpu, psx_ic_t *ic)
{
    memset(gpu, 0, sizeof(psx_gpu_t));

    gpu->io_base = PSX_GPU_BEGIN;
    gpu->io_size = PSX_GPU_SIZE;

    gpu->vram = (uint16_t *)psx_gpu_alloc(PSX_GPU_VRAM_SIZE);
    gpu->empty = psx_gpu_alloc_empty(PSX_GPU_VRAM_SIZE);

    if (gpu->vram)
        memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);

    if (gpu->empty)
        memset(gpu->empty, 0, PSX_GPU_VRAM_SIZE);

    gpu->state = GPU_STATE_RECV_CMD;
    gpu->gpustat |= 0x800000;

    gpu->in_hblank = 0;
    gpu->next_edge_fp = GPU_FP_HDRAW_NTSC;

    // Default window size, this is not normally needed
    gpu->display_mode = 1;

    gpu->skip_rows = -1;

    gpu->band_share = 256;
    gpu->band_top = 1;
    gpu->draw_ry1 = 0;
    gpu->draw_ry2 = PSX_GPU_FB_HEIGHT - 1;

    gpu->ic = ic;

#if PSXE_GPU_REMOTE
    gpu_remote_reset(gpu);
#endif
}

PSX_GPU_HOT uint32_t psx_gpu_read32(psx_gpu_t *gpu, uint32_t offset)
{
    switch (offset)
    {
    case 0x00:
    {
        uint32_t data = 0x0;

#if PSXE_GPU_REMOTE
        if (g_gpu_remote && g_gpu_stream)
        {
            if (gpu_remote_reading())
                data = gpu_remote_read_vram(gpu);
        }
        else
#endif
        if (gpu->c0_tsiz)
        {
            {
                const uint32_t i = (gpu->c0_addr + (gpu->c0_xcnt + (gpu->c0_ycnt * 1024))) & 0x7ffffu;

                data |= PSX_READ_PIX(gpu, i & 0x3ffu, i >> 10, gpu->vram[PSX_VRAM_AT(i & 0x3ffu, i >> 10)]);
            }

            gpu->c0_xcnt += 1;

            if (gpu->c0_xcnt == gpu->c0_xsiz)
            {
                gpu->c0_ycnt += 1;
                gpu->c0_xcnt = 0;
            }

            {
                const uint32_t i = (gpu->c0_addr + (gpu->c0_xcnt + (gpu->c0_ycnt * 1024))) & 0x7ffffu;

                data |= (uint32_t)PSX_READ_PIX(gpu, i & 0x3ffu, i >> 10, gpu->vram[PSX_VRAM_AT(i & 0x3ffu, i >> 10)]) << 16;
            }

            gpu->c0_xcnt += 1;

            if (gpu->c0_xcnt == gpu->c0_xsiz)
            {
                gpu->c0_ycnt += 1;
                gpu->c0_xcnt = 0;
            }

            gpu->c0_tsiz -= 2;
        }

        if (gpu->gp1_10h_req)
        {
            switch (gpu->gp1_10h_req & 7)
            {
            case 2:
            {
                data = ((gpu->texw_oy / 8) << 15) | ((gpu->texw_ox / 8) << 10) | ((gpu->texw_my / 8) << 5) | (gpu->texw_mx / 8);
            }
            break;
            case 3:
            {
                data = (gpu->draw_y1 << 10) | gpu->draw_x1;
            }
            break;
            case 4:
            {
                data = (gpu->draw_y2 << 10) | gpu->draw_x2;
            }
            break;
            case 5:
            {
                data = (gpu->off_y << 10) | gpu->off_x;
            }
            break;
            }

            gpu->gp1_10h_req = 0;
        }

        return data;
    }
    break;
    case 0x04:
        return gpu->gpustat | 0x1c000000;
    }

    log_warn("Unhandled 32-bit GPU read at offset %08x", offset);

    return 0x0;
}

PSX_GPU_HOT uint16_t psx_gpu_read16(psx_gpu_t *gpu, uint32_t offset)
{
    PRINTF("Unhandled 16-bit GPU read at offset %08lX\n", offset);

    return 0;

    // exit(1);
}

PSX_GPU_HOT uint8_t psx_gpu_read8(psx_gpu_t *gpu, uint32_t offset)
{
    PRINTF("Unhandled 8-bit GPU read at offset %08lX\n", offset);

    return 0;

    // exit(1);
}

PSX_GPU_HOT int min(int x0, int x1)
{
    return (x0 <= x1) ? x0 : x1;
}

PSX_GPU_HOT int max(int x0, int x1)
{
    return (x0 >= x1) ? x0 : x1;
}

#define EDGE(a, b, c) ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x))

/* Paletted textures need one VRAM lookup per pixel just for the palette entry.
   VRAM sits in SDRAM, where a cache line refill costs ~150 core cycles, so for
   anything bigger than a few pixels it pays to stage the palette in DTCM. */
static uint16_t PSX_GPU_DTCM_BSS __attribute__((aligned(4))) g_clut_stage[256];

static inline const uint16_t *gpu_clut_ptr(psx_gpu_t *gpu, int depth, int clutx, int cluty, uint32_t area)
{
    const uint16_t *src = &gpu->vram[PSX_VRAM_AT(clutx, cluty)];

    if (depth == 0) /* 4 bit: 16 entries */
    {
        if (area < 64u)
            return src;

        const uint32_t *s32 = (const uint32_t *)src;
        uint32_t *d32 = (uint32_t *)g_clut_stage;

        for (int i = 0; i < 8; i++)
            d32[i] = s32[i];

        return g_clut_stage;
    }

    if ((clutx + 256) > 1024)
    {
        /* A 256 entry palette that runs off the right edge of VRAM: the GPU
           goes on at the start of the same row (x is 10 bits), not in the next
           row. (Also for the reserved depth 3, which some loops read as 8 bit.) */
        const uint16_t *const row = &gpu->vram[PSX_VRAM_AT(0, cluty)];

        for (int i = 0; i < 256; i++)
            g_clut_stage[i] = row[(clutx + i) & 0x3ff];

        return g_clut_stage;
    }

    if (depth == 1) /* 8 bit: 256 entries */
    {
        if (area < 2048u)
            return src;

        const uint32_t *s32 = (const uint32_t *)src;
        uint32_t *d32 = (uint32_t *)g_clut_stage;

        for (int i = 0; i < 128; i++)
            d32[i] = s32[i];

        return g_clut_stage;
    }

    return src; /* 15 bit: no palette at all */
}

static inline __attribute__((always_inline)) uint16_t gpu_fetch_texel(psx_gpu_t *gpu, uint16_t tx, uint16_t ty, uint32_t tpx, uint32_t tpy, const uint16_t *clut, int depth)
{
    tx = (tx & ~gpu->texw_mx) | (gpu->texw_ox & gpu->texw_mx);
    ty = (ty & ~gpu->texw_my) | (gpu->texw_oy & gpu->texw_my);
    tx &= 0xff;
    ty &= 0xff;

    switch (depth)
    {
    // 4-bit
    case 0:
    {
        uint16_t texel = gpu->vram[PSX_VRAM_AT((tpx + (tx >> 2)) & 0x3ffu, tpy + ty)];

        int index = (texel >> ((tx & 0x3) << 2)) & 0xf;

        return clut[index];
    }
    break;

    // 8-bit
    case 1:
    {
        uint16_t texel = gpu->vram[PSX_VRAM_AT((tpx + (tx >> 1)) & 0x3ffu, tpy + ty)];

        int index = (texel >> ((tx & 0x1) << 3)) & 0xff;

        return clut[index];
    }
    break;

    // 15-bit
    default:
    {
        return gpu->vram[PSX_VRAM_AT((tpx + tx) & 0x3ffu, tpy + ty)];
    }
    break;
    }
}

uint16_t gpu_fetch_texel_bilinear(psx_gpu_t *gpu, float tx, float ty, uint32_t tpx, uint32_t tpy, const uint16_t *clut, int depth)
{
    float txf = floorf(tx);
    float tyf = floorf(ty);
    float txc = txf + 1.0f;
    float tyc = tyf + 1.0f;

    int s0 = gpu_fetch_texel(gpu, (int)txf, (int)tyf, tpx, tpy, clut, depth);

    if (!s0)
        return 0;

    int s1 = gpu_fetch_texel(gpu, (int)txc, (int)tyf, tpx, tpy, clut, depth);
    int s2 = gpu_fetch_texel(gpu, (int)txf, (int)tyc, tpx, tpy, clut, depth);
    int s3 = gpu_fetch_texel(gpu, (int)txc, (int)tyc, tpx, tpy, clut, depth);

    float s0r = (s0 >> 11) & 0x1f; // RGB565: Red bits 15-11
    float s0g = (s0 >> 5) & 0x3f;  // RGB565: Green bits 10-5
    float s0b = (s0 >> 0) & 0x1f;  // RGB565: Blue bits 4-0
    float s1r = (s1 >> 11) & 0x1f;
    float s1g = (s1 >> 5) & 0x3f;
    float s1b = (s1 >> 0) & 0x1f;
    float s2r = (s2 >> 11) & 0x1f;
    float s2g = (s2 >> 5) & 0x3f;
    float s2b = (s2 >> 0) & 0x1f;
    float s3r = (s3 >> 11) & 0x1f;
    float s3g = (s3 >> 5) & 0x3f;
    float s3b = (s3 >> 0) & 0x1f;

    float q1r = s0r * (txc - tx) + s1r * (tx - txf);
    float q1g = s0g * (txc - tx) + s1g * (tx - txf);
    float q1b = s0b * (txc - tx) + s1b * (tx - txf);
    float q2r = s2r * (txc - tx) + s3r * (tx - txf);
    float q2g = s2g * (txc - tx) + s3g * (tx - txf);
    float q2b = s2b * (txc - tx) + s3b * (tx - txf);
    int qr = q1r * (tyc - ty) + q2r * (ty - tyf);
    int qg = q1g * (tyc - ty) + q2g * (ty - tyf);
    int qb = q1b * (tyc - ty) + q2b * (ty - tyf);

    return (qr << 11) | (qg << 5) | qb | (s0 & 0x8000) | (s1 & 0x8000) | (s2 & 0x8000) | (s3 & 0x8000);
}

#define TL(z, a, b) \
    ((z < 0) || ((z == 0) && ((b.y > a.y) || ((b.y == a.y) && (b.x < a.x)))))

// Fast saturate for color clamping using bit operations
static inline unsigned int fast_saturate_u8(int val)
{
    if (val > 255)
        return 255;
    if (val < 0)
        return 0;
    return (unsigned int)val;
}

/*
    Semi transparency: B is what is in VRAM, F what is drawn, per 5 bit channel

      mode 0: (B + F) / 2    mode 1: B + F    mode 2: B - F    mode 3: B + F / 4

    saturated to 0..31; the mask bit of neither goes into the result.

    All three channels at once, on the packed pixels: the carries between the
    5 bit fields are taken out (or turned into the saturation) instead of
    unpacking, scaling and saturating each channel - the 15 bit pixel
    arithmetic DuckStation's software renderer uses (after blargg). Checked
    exhaustively on the host against the per channel version it replaced (which
    before 2026-09-21 multiplied modes 0 and 3 by 128 and 64 without taking its
    8.8 scale back out: every blended pixel that was not black turned white).
*/
__attribute__((always_inline)) static inline uint16_t gpu_blend_bgr555(uint32_t f, uint32_t b, int transp_mode)
{
    f &= 0x7fffu;
    b &= 0x7fffu;

    switch (transp_mode)
    {
    case 0: /* 0.5*B + 0.5*F */
        return (uint16_t)(((f + b) - ((f ^ b) & 0x0421u)) >> 1);

    case 1: /* B + F */
    {
        const uint32_t sum = f + b;
        const uint32_t carry = (sum - ((f ^ b) & 0x8421u)) & 0x8420u;

        return (uint16_t)(((sum - carry) | (carry - (carry >> 5))) & 0x7fffu);
    }

    case 2: /* B - F */
    {
        const uint32_t diff = b - f + 0x108420u;
        const uint32_t borrow = (diff - ((b ^ f) & 0x108420u)) & 0x108420u;

        return (uint16_t)(((diff - borrow) & (borrow - (borrow >> 5))) & 0x7fffu);
    }

    default: /* B + 0.25*F */
    {
        f = (f >> 2) & 0x1ce7u;

        const uint32_t sum = f + b;
        const uint32_t carry = (sum - ((f ^ b) & 0x8421u)) & 0x8420u;

        return (uint16_t)(((sum - carry) | (carry - (carry >> 5))) & 0x7fffu);
    }
    }
}

//static inline uint16_t gpu_blend_bgr555(uint16_t src, uint16_t dst, int transp_mode)
//{
//    int cr = (((src >> 11) & 0x1f) << 3) << 8;
//    int cg = (((src >> 5) & 0x3f) << 2) << 8;
//    int cb = (((src >> 0) & 0x1f) << 3) << 8;
//    const int br = (((dst >> 11) & 0x1f) << 3) << 8;
//    const int bg = (((dst >> 5) & 0x3f) << 2) << 8;
//    const int bb = (((dst >> 0) & 0x1f) << 3) << 8;
//
//    switch (transp_mode)
//    {
//    case 0:
//        cr = (br * 128 + cr * 128) >> 8;
//        cg = (bg * 128 + cg * 128) >> 8;
//        cb = (bb * 128 + cb * 128) >> 8;
//        break;
//    case 1:
//        cr = (br + cr) >> 8;
//        cg = (bg + cg) >> 8;
//        cb = (bb + cb) >> 8;
//        break;
//    case 2:
//        cr = (br - cr) >> 8;
//        cg = (bg - cg) >> 8;
//        cb = (bb - cb) >> 8;
//        break;
//    case 3:
//        cr = (br + (cr * 64)) >> 8;
//        cg = (bg + (cg * 64)) >> 8;
//        cb = (bb + (cb * 64)) >> 8;
//        break;
//    }
//
//    unsigned int ucr = fast_saturate_u8(cr);
//    unsigned int ucg = fast_saturate_u8(cg);
//    unsigned int ucb = fast_saturate_u8(cb);
//    uint32_t rgb = ucr | (ucg << 8) | (ucb << 16);
//    return rgb888_to_bgr555(rgb);
//}

#define ATTR_FRAC_BITS 12

typedef struct
{
    int32_t a;
    int32_t b;
    int32_t c;
} edge_func_t;

typedef struct
{
    int32_t dx;
    int32_t dy;
    int32_t row;
} plane_attr_t;

static inline edge_func_t edge_setup(vertex_t v0, vertex_t v1)
{
    edge_func_t edge;
    edge.a = v0.y - v1.y;
    edge.b = v1.x - v0.x;
    edge.c = (v0.x * v1.y) - (v0.y * v1.x);
    return edge;
}

static inline int32_t edge_eval(const edge_func_t *edge, int32_t x, int32_t y)
{
    return (edge->a * x) + (edge->b * y) + edge->c;
}

/*
    (num << frac_bits) / area, truncated towards zero.

    This used to be a 64 bit division - a library call of a few hundred cycles on
    this core, up to ten of them per triangle, which for the small triangles a
    character model is made of cost more than filling them. The shifted
    numerator only needs more than 32 bits for a triangle that is both very
    large and very steeply shaded, so nearly every division is a single
    hardware SDIV; the quotient is the same either way.
*/
static inline int32_t gpu_plane_div(int32_t num, int32_t area, int32_t frac_bits)
{
    const int32_t lim = (int32_t)1 << (31 - frac_bits);

    if ((num > -lim) && (num < lim))
        return (num * ((int32_t)1 << frac_bits)) / area;

    return (int32_t)(((int64_t)num * ((int64_t)1 << frac_bits)) / area);
}

static inline plane_attr_t plane_setup(const vertex_t *va,
                                       const vertex_t *vb,
                                       const vertex_t *vc,
                                       int32_t area,
                                       int attr_a,
                                       int attr_b,
                                       int attr_c,
                                       int32_t frac_bits,
                                       int32_t xmin,
                                       int32_t ymin)
{
    plane_attr_t plane;

    /* Attributes are 8 bit and coordinates 16, so the numerators fit 32 bits
       with room to spare. */
    const int32_t num_dx = attr_a * (vb->y - vc->y) +
                           attr_b * (vc->y - va->y) +
                           attr_c * (va->y - vb->y);

    const int32_t num_dy = attr_a * (vc->x - vb->x) +
                           attr_b * (va->x - vc->x) +
                           attr_c * (vb->x - va->x);

    plane.dx = gpu_plane_div(num_dx, area, frac_bits);
    plane.dy = gpu_plane_div(num_dy, area, frac_bits);

    int32_t base = attr_a << frac_bits;
    int32_t x_off = xmin - va->x;
    int32_t y_off = ymin - va->y;

    plane.row = base + plane.dx * x_off + plane.dy * y_off;
    return plane;
}

#define CLAMP(v, d, u) ((v) <= (d)) ? (d) : (((v) >= (u)) ? (u) : (v))

/*
    Narrows [start, end] (pixel indices inside a scanline's bounding box) to the
    part where one edge function is not negative.

    A triangle's intersection with a scanline is a single interval, so the span
    can be solved for instead of found by stepping the edge functions pixel by
    pixel: a rising edge (a > 0) contributes a left bound, a falling edge a
    right bound, and a constant one either nothing or an empty span. Verified
    against the stepping version over 8 million scanlines of random triangles.
*/
static inline __attribute__((always_inline)) void gpu_span_clip(int32_t e, int32_t a, int *start, int *end)
{
    if (a > 0)
    {
        if (e < 0)
        {
            const int s = (int)(((uint32_t)(-e) + (uint32_t)a - 1u) / (uint32_t)a);

            if (s > *start)
                *start = s;
        }
    }
    else if (a < 0)
    {
        if (e < 0)
        {
            *end = -1;
        }
        else
        {
            const int l = (int)((uint32_t)e / (uint32_t)(-a));

            if (l < *end)
                *end = l;
        }
    }
    else if (e < 0)
    {
        *end = -1;
    }
}

/*
    A run of one colour. The body goes out in 64 bit stores (STRD): measured on
    the board, 240 rows of 512 pixels into VRAM take 75 core cycles per cache
    line that way against 146 with 32 bit stores - the core sees whole lines
    written and does not read them from SDRAM first (write allocate), which it
    does for the 32 bit ones. But only one STRD per loop round: eight of them
    back to back (the loop unrolled) are as slow as the 32 bit stores again.
*/
static inline void gpu_fill_span(uint16_t *dst, uint16_t color, int count)
{
    if (count <= 0)
        return;

    /* halfwords up to 8 byte alignment */
    while (((uintptr_t)dst & 6u) && count)
    {
        *dst++ = color;
        --count;
    }

    const uint32_t packed = (uint32_t)color | ((uint32_t)color << 16);
    uint32_t *dst32 = (uint32_t *)dst;

#if defined(__arm__)
#pragma GCC unroll 1
    while (count >= 4)
    {
        __asm__ volatile("strd %1, %1, [%0], #8" : "+r"(dst32) : "r"(packed) : "memory");
        count -= 4;
    }
#else
    while (count >= 4)
    {
        dst32[0] = packed;
        dst32[1] = packed;
        dst32 += 2;
        count -= 4;
    }
#endif

    dst = (uint16_t *)dst32;

    while (count-- > 0)
        *dst++ = color;
}

//void gpu_render_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge)
//{
//    vertex_t a, b, c;
//    const int tpx = (data.texp & 0xf) << 6;
//    const int tpy = (data.texp & 0x10) << 4;
//    const int clutx = (data.clut & 0x3f) << 4;
//    const int cluty = (data.clut >> 6) & 0x1ff;
//    const int depth = (data.texp >> 7) & 3;
//    const int is_textured = (data.attrib & PA_TEXTURED) != 0;
//    const int is_shaded = (data.attrib & PA_SHADED) != 0;
//    const int is_raw = (data.attrib & PA_RAW) != 0;
//    const int transparency_enabled = (data.attrib & PA_TRANSP) != 0;
//    const int transp_mode = is_textured ? ((data.texp >> 5) & 3) : ((gpu->gpustat >> 5) & 3);
//
//    a = v0;
//    if (EDGE(v0, v1, v2) < 0)
//    {
//        b = v2;
//        c = v1;
//    }
//    else
//    {
//        b = v1;
//        c = v2;
//    }
//
//    const int off_x = gpu->off_x;
//    const int off_y = gpu->off_y;
//    a.x += off_x;
//    b.x += off_x;
//    c.x += off_x;
//    a.y += off_y;
//    b.y += off_y;
//    c.y += off_y;
//
//    int xmin = max(min3(a.x, b.x, c.x), gpu->draw_x1);
//    int ymin = max(min3(a.y, b.y, c.y), gpu->draw_ry1);
//    int xmax = min(max3(a.x, b.x, c.x), min(gpu->draw_x2, 1023));
//    int ymax = min(max3(a.y, b.y, c.y), min(gpu->draw_ry2, 511));
//
//    if (xmin > xmax || ymin > ymax)
//        return;
//
//    int64_t area64 = (int64_t)(b.x - a.x) * (int64_t)(c.y - a.y) - (int64_t)(b.y - a.y) * (int64_t)(c.x - a.x);
//    if (area64 <= 0)
//        return;
//    int32_t area = (int32_t)area64;
//
//    edge_func_t edge0 = edge_setup(a, b);
//    edge_func_t edge1 = edge_setup(b, c);
//    edge_func_t edge2 = edge_setup(c, a);
//
//    int32_t e0_row = edge_eval(&edge0, xmin, ymin);
//    int32_t e1_row = edge_eval(&edge1, xmin, ymin);
//    int32_t e2_row = edge_eval(&edge2, xmin, ymin);
//
//    uint16_t *__restrict vram = gpu->vram;
//    const uint32_t flat_color = data.v[0].c;
//    const uint16_t flat_color555 = rgb888_to_bgr555(flat_color);
//    const int32_t edge0_a = edge0.a;
//    const int32_t edge1_a = edge1.a;
//    const int32_t edge2_a = edge2.a;
//    const int32_t edge0_b = edge0.b;
//    const int32_t edge1_b = edge1.b;
//    const int32_t edge2_b = edge2.b;
//
//    if (!is_textured && !is_shaded && !transparency_enabled)
//    {
//        const uint16_t out_color = flat_color555;
//        int vram_row = ymin * 1024;
//
//        for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
//        {
//            int32_t e0 = e0_row;
//            int32_t e1 = e1_row;
//            int32_t e2 = e2_row;
//            int x = xmin;
//
//            while (x <= xmax && (e0 | e1 | e2) < 0)
//            {
//                e0 += edge0_a;
//                e1 += edge1_a;
//                e2 += edge2_a;
//                ++x;
//            }
//
//            if (x <= xmax)
//            {
//                int start_x = x;
//                int count = 0;
//                do
//                {
//                    count++;
//                    e0 += edge0_a;
//                    e1 += edge1_a;
//                    e2 += edge2_a;
//                    ++x;
//                } while (x <= xmax && (e0 | e1 | e2) >= 0);
//                gpu_fill_span(&vram[vram_row + start_x], out_color, count);
//            }
//
//            e0_row += edge0_b;
//            e1_row += edge1_b;
//            e2_row += edge2_b;
//        }
//        return;
//    }
//
//    plane_attr_t r_plane = {0}, g_plane = {0}, b_plane = {0};
//    plane_attr_t tx_plane = {0}, ty_plane = {0};
//    int32_t r_row = 0, g_row = 0, b_row = 0;
//    int32_t tx_row = 0, ty_row = 0;
//
//    if (is_shaded)
//    {
//        r_plane = plane_setup(&a, &b, &c, area, (a.c >> 0) & 0xff, (b.c >> 0) & 0xff, (c.c >> 0) & 0xff, ATTR_FRAC_BITS, xmin, ymin);
//        g_plane = plane_setup(&a, &b, &c, area, (a.c >> 8) & 0xff, (b.c >> 8) & 0xff, (c.c >> 8) & 0xff, ATTR_FRAC_BITS, xmin, ymin);
//        b_plane = plane_setup(&a, &b, &c, area, (a.c >> 16) & 0xff, (b.c >> 16) & 0xff, (c.c >> 16) & 0xff, ATTR_FRAC_BITS, xmin, ymin);
//        r_row = r_plane.row;
//        g_row = g_plane.row;
//        b_row = b_plane.row;
//    }
//
//    if (is_textured)
//    {
//        tx_plane = plane_setup(&a, &b, &c, area, a.tx, b.tx, c.tx, ATTR_FRAC_BITS, xmin, ymin);
//        ty_plane = plane_setup(&a, &b, &c, area, a.ty, b.ty, c.ty, ATTR_FRAC_BITS, xmin, ymin);
//        tx_row = tx_plane.row;
//        ty_row = ty_plane.row;
//    }
//
//    if (!is_textured && !transparency_enabled)
//    {
//        int vram_row = ymin * 1024;
//        for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
//        {
//            int32_t e0 = e0_row;
//            int32_t e1 = e1_row;
//            int32_t e2 = e2_row;
//            int32_t r_val = r_row;
//            int32_t g_val = g_row;
//            int32_t b_val = b_row;
//            uint16_t *dst = &vram[vram_row + xmin];
//            int x = xmin;
//            const int kernel_row = ((y - ymin) & 3) << 2;
//            int dx_dither = 0;
//
//            while (x <= xmax && (e0 | e1 | e2) < 0)
//            {
//                e0 += edge0_a;
//                e1 += edge1_a;
//                e2 += edge2_a;
//                r_val += r_plane.dx;
//                g_val += g_plane.dx;
//                b_val += b_plane.dx;
//                ++x;
//                ++dst;
//                dx_dither = (dx_dither + 1) & 3;
//            }
//
//            while (x <= xmax && (e0 | e1 | e2) >= 0)
//            {
//                int cr = r_val >> ATTR_FRAC_BITS;
//                int cg = g_val >> ATTR_FRAC_BITS;
//                int cb = b_val >> ATTR_FRAC_BITS;
//                const int dither = g_psx_gpu_dither_kernel[dx_dither | kernel_row];
//                cr = fast_saturate_u8(cr + dither);
//                cg = fast_saturate_u8(cg + dither);
//                cb = fast_saturate_u8(cb + dither);
//                const uint32_t mod_color = (cb << 16) | (cg << 8) | cr;
//                *dst++ = rgb888_to_bgr555(mod_color);
//
//                e0 += edge0_a;
//                e1 += edge1_a;
//                e2 += edge2_a;
//                r_val += r_plane.dx;
//                g_val += g_plane.dx;
//                b_val += b_plane.dx;
//                ++x;
//                dx_dither = (dx_dither + 1) & 3;
//            }
//
//            e0_row += edge0_b;
//            e1_row += edge1_b;
//            e2_row += edge2_b;
//            r_row += r_plane.dy;
//            g_row += g_plane.dy;
//            b_row += b_plane.dy;
//        }
//        return;
//    }
//
//    if (!is_textured)
//    {
//        if (is_shaded)
//        {
//            int vram_row = ymin * 1024;
//            for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
//            {
//                int32_t e0 = e0_row;
//                int32_t e1 = e1_row;
//                int32_t e2 = e2_row;
//                int32_t r_val = r_row;
//                int32_t g_val = g_row;
//                int32_t b_val = b_row;
//                uint16_t *dst = &vram[vram_row + xmin];
//                int x = xmin;
//                const int kernel_row = ((y - ymin) & 3) << 2;
//                int dx_dither = 0;
//
//                while (x <= xmax && (e0 | e1 | e2) < 0)
//                {
//                    e0 += edge0_a;
//                    e1 += edge1_a;
//                    e2 += edge2_a;
//                    r_val += r_plane.dx;
//                    g_val += g_plane.dx;
//                    b_val += b_plane.dx;
//                    ++x;
//                    ++dst;
//                    dx_dither = (dx_dither + 1) & 3;
//                }
//
//                while (x <= xmax && (e0 | e1 | e2) >= 0)
//                {
//                    int cr = r_val >> ATTR_FRAC_BITS;
//                    int cg = g_val >> ATTR_FRAC_BITS;
//                    int cb = b_val >> ATTR_FRAC_BITS;
//                    const int dither = g_psx_gpu_dither_kernel[dx_dither | kernel_row];
//                    cr = fast_saturate_u8(cr + dither);
//                    cg = fast_saturate_u8(cg + dither);
//                    cb = fast_saturate_u8(cb + dither);
//                    const uint32_t mod_color = (cb << 16) | (cg << 8) | cr;
//                    const uint16_t out_color = rgb888_to_bgr555(mod_color);
//                    *dst = gpu_blend_bgr555(out_color, *dst, transp_mode);
//
//                    e0 += edge0_a;
//                    e1 += edge1_a;
//                    e2 += edge2_a;
//                    r_val += r_plane.dx;
//                    g_val += g_plane.dx;
//                    b_val += b_plane.dx;
//                    ++x;
//                    ++dst;
//                    dx_dither = (dx_dither + 1) & 3;
//                }
//
//                e0_row += edge0_b;
//                e1_row += edge1_b;
//                e2_row += edge2_b;
//                r_row += r_plane.dy;
//                g_row += g_plane.dy;
//                b_row += b_plane.dy;
//            }
//        }
//        else
//        {
//            int vram_row = ymin * 1024;
//            for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
//            {
//                int32_t e0 = e0_row;
//                int32_t e1 = e1_row;
//                int32_t e2 = e2_row;
//                uint16_t *dst = &vram[vram_row + xmin];
//                int x = xmin;
//
//                while (x <= xmax && (e0 | e1 | e2) < 0)
//                {
//                    e0 += edge0_a;
//                    e1 += edge1_a;
//                    e2 += edge2_a;
//                    ++x;
//                    ++dst;
//                }
//
//                while (x <= xmax && (e0 | e1 | e2) >= 0)
//                {
//                    *dst = gpu_blend_bgr555(flat_color555, *dst, transp_mode);
//
//                    e0 += edge0_a;
//                    e1 += edge1_a;
//                    e2 += edge2_a;
//                    ++x;
//                    ++dst;
//                }
//
//                e0_row += edge0_b;
//                e1_row += edge1_b;
//                e2_row += edge2_b;
//            }
//        }
//        return;
//    }
//
//    if (is_shaded)
//    {
//        int vram_row = ymin * 1024;
//        for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
//        {
//            int32_t e0 = e0_row;
//            int32_t e1 = e1_row;
//            int32_t e2 = e2_row;
//            int32_t r_val = r_row;
//            int32_t g_val = g_row;
//            int32_t b_val = b_row;
//            int32_t tx_val = tx_row;
//            int32_t ty_val = ty_row;
//            uint16_t *dst = &vram[vram_row + xmin];
//            int x = xmin;
//            const int kernel_row = ((y - ymin) & 3) << 2;
//            int dx_dither = 0;
//
//            while (x <= xmax && (e0 | e1 | e2) < 0)
//            {
//                e0 += edge0_a;
//                e1 += edge1_a;
//                e2 += edge2_a;
//                r_val += r_plane.dx;
//                g_val += g_plane.dx;
//                b_val += b_plane.dx;
//                tx_val += tx_plane.dx;
//                ty_val += ty_plane.dx;
//                ++x;
//                ++dst;
//                dx_dither = (dx_dither + 1) & 3;
//            }
//
//            while (x <= xmax && (e0 | e1 | e2) >= 0)
//            {
//                int cr = r_val >> ATTR_FRAC_BITS;
//                int cg = g_val >> ATTR_FRAC_BITS;
//                int cb = b_val >> ATTR_FRAC_BITS;
//                const int dither = g_psx_gpu_dither_kernel[dx_dither | kernel_row];
//                cr = fast_saturate_u8(cr + dither);
//                cg = fast_saturate_u8(cg + dither);
//                cb = fast_saturate_u8(cb + dither);
//                const uint32_t mod_color = (cb << 16) | (cg << 8) | cr;
//
//                const int tx = tx_val >> ATTR_FRAC_BITS;
//                const int ty = ty_val >> ATTR_FRAC_BITS;
//                const uint16_t texel = gpu_fetch_texel(gpu, tx, ty, tpx, tpy, clutx, cluty, depth);
//
//                if (__builtin_expect(texel != 0, 1))
//                {
//                    uint16_t out_color;
//                    if (is_raw)
//                    {
//                        out_color = texel;
//                    }
//                    else
//                    {
//                        const int tr = ((texel >> 0) & 0x1f) << 3;
//                        const int tg = ((texel >> 5) & 0x1f) << 3;
//                        const int tb = ((texel >> 10) & 0x1f) << 3;
//                        const int mr = (mod_color >> 0) & 0xff;
//                        const int mg = (mod_color >> 8) & 0xff;
//                        const int mb = (mod_color >> 16) & 0xff;
//                        const int pr = (tr * mr) >> 7;
//                        const int pg = (tg * mg) >> 7;
//                        const int pb = (tb * mb) >> 7;
//                        const unsigned int upr = fast_saturate_u8(pr);
//                        const unsigned int upg = fast_saturate_u8(pg);
//                        const unsigned int upb = fast_saturate_u8(pb);
//                        const uint32_t rgb = upr | (upg << 8) | (upb << 16);
//                        out_color = rgb888_to_bgr555(rgb);
//                    }
//
//                    if (transparency_enabled && (texel & 0x8000))
//                    {
//                        *dst = gpu_blend_bgr555(out_color, *dst, transp_mode);
//                    }
//                    else
//                    {
//                        *dst = out_color;
//                    }
//                }
//
//                e0 += edge0_a;
//                e1 += edge1_a;
//                e2 += edge2_a;
//                r_val += r_plane.dx;
//                g_val += g_plane.dx;
//                b_val += b_plane.dx;
//                tx_val += tx_plane.dx;
//                ty_val += ty_plane.dx;
//                ++x;
//                ++dst;
//                dx_dither = (dx_dither + 1) & 3;
//            }
//
//            e0_row += edge0_b;
//            e1_row += edge1_b;
//            e2_row += edge2_b;
//            r_row += r_plane.dy;
//            g_row += g_plane.dy;
//            b_row += b_plane.dy;
//            tx_row += tx_plane.dy;
//            ty_row += ty_plane.dy;
//        }
//        return;
//    }
//
//    const int mod_r = flat_color & 0xff;
//    const int mod_g = (flat_color >> 8) & 0xff;
//    const int mod_b = (flat_color >> 16) & 0xff;
//    int vram_row = ymin * 1024;
//
//    for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
//    {
//        int32_t e0 = e0_row;
//        int32_t e1 = e1_row;
//        int32_t e2 = e2_row;
//        int32_t tx_val = tx_row;
//        int32_t ty_val = ty_row;
//        uint16_t *dst = &vram[vram_row + xmin];
//        int x = xmin;
//
//        while (x <= xmax && (e0 | e1 | e2) < 0)
//        {
//            e0 += edge0_a;
//            e1 += edge1_a;
//            e2 += edge2_a;
//            tx_val += tx_plane.dx;
//            ty_val += ty_plane.dx;
//            ++x;
//            ++dst;
//        }
//
//        while (x <= xmax && (e0 | e1 | e2) >= 0)
//        {
//            const int tx = tx_val >> ATTR_FRAC_BITS;
//            const int ty = ty_val >> ATTR_FRAC_BITS;
//            const uint16_t texel = gpu_fetch_texel(gpu, tx, ty, tpx, tpy, clutx, cluty, depth);
//
//            if (__builtin_expect(texel != 0, 1))
//            {
//                uint16_t out_color;
//                if (is_raw)
//                {
//                    out_color = texel;
//                }
//                else
//                {
//                    const int tr = ((texel >> 0) & 0x1f) << 3;
//                    const int tg = ((texel >> 5) & 0x1f) << 3;
//                    const int tb = ((texel >> 10) & 0x1f) << 3;
//                    const int pr = (tr * mod_r) >> 7;
//                    const int pg = (tg * mod_g) >> 7;
//                    const int pb = (tb * mod_b) >> 7;
//                    const unsigned int upr = fast_saturate_u8(pr);
//                    const unsigned int upg = fast_saturate_u8(pg);
//                    const unsigned int upb = fast_saturate_u8(pb);
//                    const uint32_t rgb = upr | (upg << 8) | (upb << 16);
//                    out_color = rgb888_to_bgr555(rgb);
//                }
//
//                if (transparency_enabled && (texel & 0x8000))
//                {
//                    *dst = gpu_blend_bgr555(out_color, *dst, transp_mode);
//                }
//                else
//                {
//                    *dst = out_color;
//                }
//            }
//
//            e0 += edge0_a;
//            e1 += edge1_a;
//            e2 += edge2_a;
//            tx_val += tx_plane.dx;
//            ty_val += ty_plane.dx;
//            ++x;
//            ++dst;
//        }
//
//        e0_row += edge0_b;
//        e1_row += edge1_b;
//        e2_row += edge2_b;
//        tx_row += tx_plane.dy;
//        ty_row += ty_plane.dy;
//    }
//}

/* VRAM holds textures in the PSX native BGR555 layout while rendered pixels are
   stored in the RGB565 layout the LCD expects, so an unmodulated texel only
   needs the channel swap - not the full modulate/saturate round trip. */
__attribute__((always_inline)) static inline uint16_t modulate_bgr555(uint16_t texel, uint8_t mod_r, uint8_t mod_g, uint8_t mod_b)
{
    // Extract RGB555 and expand to 8-bit - EXACT match to original
    uint32_t tr = ((texel >> 0) & 0x1f) << 3;
    uint32_t tg = ((texel >> 5) & 0x1f) << 3;
    uint32_t tb = ((texel >> 10) & 0x1f) << 3;

    // Modulate - compiler will optimize this well in Release builds
    uint32_t pr = (tr * mod_r) >> 7;
    uint32_t pg = (tg * mod_g) >> 7;
    uint32_t pb = (tb * mod_b) >> 7;

    // Saturate to 8-bit
    if (pr > 255) pr = 255;
    if (pg > 255) pg = 255;
    if (pb > 255) pb = 255;

    /* the mask bit of the texel is carried over, as the hardware does */
    return (uint16_t)((pr >> 3) | ((pg >> 3) << 5) | ((pb >> 3) << 10) | (texel & 0x8000u));
}

/*
    Textured spans, one small function per kind (texel depth x shading x semi
    transparency), the way DuckStation's software renderer has a DrawSpan per
    mode. They used to be loops expanded inside the one big triangle function,
    and there the compiler kept almost nothing in registers: a Gouraud textured
    pixel took about a hundred instructions, a dozen of them loads of loop
    invariants from the stack - some 100 cycles per pixel, measured. On their
    own, each loop has the registers to itself.

    Only for primitives without a texture window (nearly all of them): the
    texture coordinate is then just the integer part of the 8.12 value, taken
    straight out of it, and the texel address follows from two bit fields.

    The Gouraud textured kind goes in two passes over up to 64 pixels at a time -
    the colours, dithered and clamped, into a buffer, then the texels modulated
    by them - because the interpolants of both together do not fit the core's
    registers either. The arithmetic is that of the loops this replaces, bit for
    bit; see modulate_bgr555 and PSXE_CH.
*/
typedef struct
{
    const uint16_t *tex;  /* the texture page: &vram[PSX_VRAM_AT(texture page x, y)] */
    const uint16_t *clut;
    int32_t du, dv;       /* texture coordinate steps, 8.ATTR_FRAC_BITS */
    int32_t dr, dg, db;   /* colour steps (Gouraud) */
    uint32_t mr, mg, mb;  /* modulation colour (flat) */
    uint32_t dither;      /* the span's dither values from its first pixel on, a signed byte each */
    int transp_mode;
    int exact;            /* the polygon covers its own texture page: no read ahead (page_hit) */
} gpu_span_t;

typedef void (*gpu_span_fn_t)(const gpu_span_t *s, uint16_t *dst, int n, uint32_t u, uint32_t v,
                              int32_t r, int32_t g, int32_t b);

/*
    x >> s saturated to 0 .. 2^bits - 1: one USAT with its shift. Written out,
    because GCC does not make that instruction of a clamp whose lower bound it
    can prove never applies - it leaves a compare, an IT and a move per channel.
*/
#if defined(__arm__) && defined(__ARM_ARCH_7EM__)
#define GPU_USAT_ASR(x, bits, s)                                                   \
    __extension__({                                                                \
        uint32_t usat_r_;                                                          \
        __asm__("usat %0, #" #bits ", %1, asr #" #s : "=r"(usat_r_) : "r"((int32_t)(x))); \
        usat_r_;                                                                   \
    })
#else
#define GPU_USAT_ASR(x, bits, s)                                                   \
    __extension__({                                                                \
        const int32_t usat_v_ = (int32_t)(x) >> (s);                               \
        (usat_v_ < 0) ? 0u : ((usat_v_ > ((1 << (bits)) - 1)) ? (uint32_t)((1 << (bits)) - 1) : (uint32_t)usat_v_); \
    })
#endif

/* one channel of modulate_bgr555: ((t5 << 3) * m) >> 7 saturated to 8 bits, back to 5 bits - which
   is (t5 * m) >> 7 saturated to 5 bits, one USAT with its shift (the lower clamp never applies) */
__attribute__((always_inline)) static inline uint32_t gpu_mod_ch(uint32_t t5, uint32_t m)
{
    return GPU_USAT_ASR(t5 * m, 5, 7);
}

__attribute__((always_inline)) static inline uint32_t gpu_mod_px(uint32_t t, uint32_t mr, uint32_t mg, uint32_t mb)
{
    return gpu_mod_ch(t & 0x1fu, mr) | (gpu_mod_ch((t >> 5) & 0x1fu, mg) << 5) |
           (gpu_mod_ch((t >> 10) & 0x1fu, mb) << 10) | (t & 0x8000u);
}

/* the texel at 8.12 coordinates u, v: 4 bit, 8 bit (through the palette) or 15 bit */
__attribute__((always_inline)) static inline uint32_t gpu_span_texel(const uint16_t *tex, const uint16_t *clut,
                                                                     uint32_t u, uint32_t v, const int F)
{
    const uint32_t row = ((v >> ATTR_FRAC_BITS) & 0xffu) * PSX_GPU_VRAM_PITCH;

    if (F == 0)
        return clut[(tex[row + ((u >> (ATTR_FRAC_BITS + 2)) & 0x3fu)] >> ((u >> (ATTR_FRAC_BITS - 2)) & 0xcu)) & 0xfu];

    if (F == 1)
        return clut[(tex[row + ((u >> (ATTR_FRAC_BITS + 1)) & 0x7fu)] >> ((u >> (ATTR_FRAC_BITS - 3)) & 0x8u)) & 0xffu];

    return tex[row + ((u >> ATTR_FRAC_BITS) & 0xffu)];
}

/* a colour channel for Gouraud modulation: 8.12 plus the dither offset, clamped to 8 bits (PSXE_CH) */
__attribute__((always_inline)) static inline uint32_t gpu_span_ch(int32_t v, int32_t d)
{
    const int32_t c = (v >> ATTR_FRAC_BITS) + d;

    return (c < 0) ? 0u : ((c > 255) ? 255u : (uint32_t)c);
}

/*
    Cache line fills asked for ahead of a 4 bit span: the texture lines along it, every 8th
    pixel's, and when it blends the destination's lines, which it reads. The core fills one
    line at a time and a PLD waits for the one before it (measured: a PLD alone costs nothing,
    each further one about a line fill), so this only moves the stalls out of the span - where a
    missing line stalls longer, the load waiting behind it - unless there is work to do in the
    meantime (gpu_span_g4's colour pass). Right in front of a short span it did not pay; spread
    over the colour pass one per 8 pixels, the pass itself got slower than the stalls saved.
*/
__attribute__((always_inline)) static inline void gpu_span_prefetch4(const uint16_t *tex, const uint16_t *dst, int n,
                                                                     uint32_t u, uint32_t v, uint32_t du, uint32_t dv,
                                                                     const int TRANSP)
{
#pragma GCC unroll 1
    for (int i = 0; i < n; i += 8)
    {
        const uint32_t pu = u + du * (uint32_t)i, pv = v + dv * (uint32_t)i;

        __builtin_prefetch(
            &tex[((pv >> ATTR_FRAC_BITS) & 0xffu) * PSX_GPU_VRAM_PITCH + ((pu >> (ATTR_FRAC_BITS + 2)) & 0x3fu)]);
    }

    if (TRANSP)
    {
        const uintptr_t last = (uintptr_t)(dst + n - 1);

#pragma GCC unroll 1
        for (uintptr_t p = (uintptr_t)dst & ~(uintptr_t)31; p <= last; p += 32u)
            __builtin_prefetch((const void *)p);
    }
}

__attribute__((always_inline)) static inline void gpu_span_tex(const gpu_span_t *s, uint16_t *dst, int n,
                                                               uint32_t u, uint32_t v, int32_t r, int32_t g,
                                                               int32_t b, const int F, const int SH,
                                                               const int TRANSP)
{
    const uint16_t *const tex = s->tex;
    const uint16_t *const clut = s->clut;
    const uint32_t du = (uint32_t)s->du;
    const uint32_t dv = (uint32_t)s->dv;
    const int transp_mode = s->transp_mode;

    if (SH != 2)
    {
        const uint32_t mr = s->mr, mg = s->mg, mb = s->mb;

        /* unrolled, the loop no longer fits the registers */
#pragma GCC unroll 1
        do
        {
            const uint32_t t = gpu_span_texel(tex, clut, u, v, F);

            if (t)
            {
                const uint32_t out = (SH == 0) ? t : gpu_mod_px(t, mr, mg, mb);

                *dst = (uint16_t)((TRANSP && (t & 0x8000u)) ? gpu_blend_bgr555(out, *dst, transp_mode) : out);
            }

            ++dst;
            u += du;
            v += dv;
        }
        while (--n > 0);

        return;
    }

    const int32_t dr = s->dr, dg = s->dg, db = s->db;
    uint32_t dither = s->dither;

    while (n > 0)
    {
        /* red and green of a pixel in one word, red in the low half, and blue 64
           words further on: the modulation multiplies 16 bit halves (SMULBB,
           SMULBT), so nothing has to be taken apart per pixel */
        uint32_t col[128];
        const int chunk = (n < 64) ? n : 64;

        for (int i = 0; i < chunk; i++)
        {
            const int32_t d = (int32_t)(int8_t)(dither & 0xffu);

            dither = (dither >> 8) | (dither << 24);

            col[i] = gpu_span_ch(r, d) | (gpu_span_ch(g, d) << 16);
            col[64 + i] = gpu_span_ch(b, d);

            r += dr;
            g += dg;
            b += db;
        }

        const uint32_t *c = col;
        uint16_t *const end = dst + chunk;

#pragma GCC unroll 1
        do
        {
            const uint32_t t = gpu_span_texel(tex, clut, u, v, F);
            const int32_t rg = (int32_t)c[0];
            const int32_t bl = (int32_t)c[64];

            u += du;
            v += dv;
            c++;

            if (t)
            {
                uint32_t out = GPU_USAT_ASR((int32_t)(int16_t)(t & 0x1fu) * (int32_t)(int16_t)rg, 5, 7) |
                               (GPU_USAT_ASR((int32_t)(int16_t)((t >> 5) & 0x1fu) * (rg >> 16), 5, 7) << 5) |
                               (GPU_USAT_ASR((int32_t)((t >> 10) & 0x1fu) * bl, 5, 7) << 10);

                /* the mask bit goes through unless the pixel is blended, which drops it -
                   and with semi transparency a texel with the bit set is always blended */
                if (TRANSP && (t & 0x8000u))
                    *dst = gpu_blend_bgr555(out, *dst, transp_mode);
                else
                    *dst = (uint16_t)(TRANSP ? out : (out | (t & 0x8000u)));
            }

            dst++;
        }
        while (dst != end);

        n -= chunk;
    }
}

/*
    Gouraud shading with a 4 bit texture, the general case (the colour changes
    across the polygon - Atlantis' lit walls and floors).

    The palette is taken apart once per polygon (gpu_split_make): per entry red
    and green in the two halves of one word, blue in the top half of another,
    whose low half keeps the mask bit (bit 0) and a "drawn" bit (1) - so a
    pixel's three products are SMULBB, SMULTT and SMULTB straight from the
    words, with nothing unpacked per pixel, and a texel of 0 is a zero word.

    CL 0: the span's colours stay within 4 .. 251 (checked at both ends - the
    colour is linear along it), so colour + dither needs no clamping and red and
    green go into one word with a PKHBT. The arithmetic is gpu_span_tex's, bit
    for bit.

    Two passes over up to 64 pixels: the colours into g_gpu_g4_col (red | green
    and blue of a pixel side by side, one LDRD), then texel and shading. The
    chunk's colours end at the end of the buffer, which is 512 byte aligned: the
    shading loop ends when its pointer gets there and needs no end register.
*/
static uint32_t PSX_GPU_DTCM_BSS __attribute__((aligned(8))) g_gpu_split[16][2];
static uint32_t PSX_GPU_DTCM_BSS g_gpu_split_key[9]; /* valid, then the 16 palette entries */
static uint32_t PSX_GPU_DTCM_BSS __attribute__((aligned(512))) g_gpu_g4_col[128];

/* a palette's 16 entries as 8 words, and whether two such are the same - by hand: the project builds
   with -fno-builtin, so memcpy and memcmp are calls, into flash */
typedef uint32_t __attribute__((may_alias, aligned(2))) gpu_u32_16_t;

static inline void gpu_pal_load(uint32_t *pal, const uint16_t *clut)
{
    const gpu_u32_16_t *c = (const gpu_u32_16_t *)clut;

    for (uint32_t i = 0; i < 8u; i++)
        pal[i] = c[i];
}

static inline int gpu_pal_same(const uint32_t *a, const uint32_t *b)
{
    uint32_t d = 0;

    for (uint32_t i = 0; i < 8u; i++)
        d |= a[i] ^ b[i];

    return !d;
}

static PSX_GPU_RAS void gpu_split_make(const uint16_t *clut)
{
    uint32_t pal[8];

    gpu_pal_load(pal, clut);

    if (g_gpu_split_key[0] && gpu_pal_same(&g_gpu_split_key[1], pal))
        return;

    g_gpu_split_key[0] = 1u;
    gpu_pal_load(&g_gpu_split_key[1], clut);

    for (uint32_t i = 0; i < 16u; i++)
    {
        const uint32_t t = clut[i];

        g_gpu_split[i][0] = (t & 0x1fu) | (((t >> 5) & 0x1fu) << 16);
        g_gpu_split[i][1] = t ? ((((t >> 10) & 0x1fu) << 16) | 2u | (t >> 15)) : 0u;
    }
}

#if defined(__arm__) && (PSX_GPU_VRAM_PITCH == 1040)
#define GPU_G4_ASM 1

typedef struct
{
    uint32_t u, v, du, dv;
    const uint16_t *tex;
    const uint32_t *split; /* g_gpu_split */
    const uint32_t *col;   /* the chunk's first pixel in g_gpu_g4_col */
    uint16_t *dst;
} gpu_g4_loop_t;

/*
    The shading pass of gpu_span_g4 without blending, scheduled by hand: GCC's
    version stalled on nearly every result (the texel load, each product before
    its USAT) and ran at under one instruction per cycle. Here the next pixel's
    texel is loaded a whole pixel ahead and the three products are started back
    to back. Registers: r0 u, r1 v, r2 du, r3 dv, r4 texture page, r5 palette
    words, r6 colours, r7 destination, r8 the texel word ahead, r9-r12 / lr work.
*/
static PSX_GPU_RAS __attribute__((naked, noinline)) void gpu_g4_loop_o(const gpu_g4_loop_t *p)
{
    (void)p;

    __asm__ volatile(
        "push   {r4-r11, lr}\n\t"
        "ldm    r0, {r0-r7}\n\t"
        /* the first pixel's texel word */
        "ubfx   r9, r1, #12, #8\n\t"
        "ubfx   r8, r0, #14, #6\n\t"
        "add    r9, r9, r9, lsl #6\n\t"
        "add    r8, r8, r9, lsl #4\n\t"
        "ldrh   r8, [r4, r8, lsl #1]\n\t"
        "1:\n\t"
        /* this pixel's palette words; the next pixel's coordinates */
        "lsr    r9, r0, #10\n\t"
        "and    r9, r9, #12\n\t"
        "lsr    r9, r8, r9\n\t"
        "add    r0, r0, r2\n\t"
        "and    r9, r9, #15\n\t"
        "add    r1, r1, r3\n\t"
        "add    r9, r5, r9, lsl #3\n\t"
        "ubfx   r11, r1, #12, #8\n\t"
        "ldrd   r12, lr, [r9]\n\t"
        "ubfx   r8, r0, #14, #6\n\t"
        "add    r11, r11, r11, lsl #6\n\t"
        "ldrd   r9, r10, [r6], #8\n\t"
        "add    r8, r8, r11, lsl #4\n\t"
        /* the next pixel's texel word (in the page: the coordinates are masked) */
        "ldrh   r8, [r4, r8, lsl #1]\n\t"
        "cmp    lr, #0\n\t"
        "beq    2f\n\t"
        "smultb r10, lr, r10\n\t"
        "smulbb r11, r12, r9\n\t"
        "smultt r9, r12, r9\n\t"
        "usat   r10, #5, r10, asr #7\n\t"
        "usat   r11, #5, r11, asr #7\n\t"
        "usat   r9, #5, r9, asr #7\n\t"
        "bfi    r10, lr, #5, #1\n\t"
        "orr    r11, r11, r9, lsl #5\n\t"
        "orr    r11, r11, r10, lsl #10\n\t"
        "strh   r11, [r7]\n\t"
        "2:\n\t"
        "adds   r7, r7, #2\n\t"
        "lsls   r9, r6, #23\n\t"
        "bne    1b\n\t"
        "pop    {r4-r11, pc}\n\t");
}

/* The same with blending mode 1 (B + F: Atlantis' light and glow) or 3 (B + F / 4: QUARTER takes F
   down to a quarter per field first, (F >> 2) & 0x1ce7 as in gpu_blend_bgr555): a texel with its mask
   bit is added to the destination, per 5 bit field saturated; one without is stored as it is, without
   the mask bit */
#define GPU_G4_LOOP_T(NAME, QUARTER)                                                      \
    static PSX_GPU_RAS __attribute__((naked, noinline)) void NAME(const gpu_g4_loop_t *p) \
    {                                                                                     \
        (void)p;                                                                          \
                                                                                          \
        __asm__ volatile(                                                                 \
            "push   {r4-r11, lr}\n\t"                                                     \
            "ldm    r0, {r0-r7}\n\t"                                                      \
            "ubfx   r9, r1, #12, #8\n\t"                                                  \
            "ubfx   r8, r0, #14, #6\n\t"                                                  \
            "add    r9, r9, r9, lsl #6\n\t"                                               \
            "add    r8, r8, r9, lsl #4\n\t"                                               \
            "ldrh   r8, [r4, r8, lsl #1]\n\t"                                             \
            "1:\n\t"                                                                      \
            "lsr    r9, r0, #10\n\t"                                                      \
            "and    r9, r9, #12\n\t"                                                      \
            "lsr    r9, r8, r9\n\t"                                                       \
            "add    r0, r0, r2\n\t"                                                       \
            "and    r9, r9, #15\n\t"                                                      \
            "add    r1, r1, r3\n\t"                                                       \
            "add    r9, r5, r9, lsl #3\n\t"                                               \
            "ubfx   r11, r1, #12, #8\n\t"                                                 \
            "ldrd   r12, lr, [r9]\n\t"                                                    \
            "ubfx   r8, r0, #14, #6\n\t"                                                  \
            "add    r11, r11, r11, lsl #6\n\t"                                            \
            "ldrd   r9, r10, [r6], #8\n\t"                                                \
            "add    r8, r8, r11, lsl #4\n\t"                                              \
            "ldrh   r8, [r4, r8, lsl #1]\n\t"                                             \
            "cmp    lr, #0\n\t"                                                           \
            "beq    3f\n\t"                                                               \
            "smultb r10, lr, r10\n\t"                                                     \
            "smulbb r11, r12, r9\n\t"                                                     \
            "smultt r9, r12, r9\n\t"                                                      \
            "usat   r10, #5, r10, asr #7\n\t"                                             \
            "usat   r11, #5, r11, asr #7\n\t"                                             \
            "usat   r9, #5, r9, asr #7\n\t"                                               \
            "orr    r11, r11, r9, lsl #5\n\t"                                             \
            "orr    r11, r11, r10, lsl #10\n\t"                                           \
            "tst    lr, #1\n\t"                                                           \
            "beq    2f\n\t"                                                               \
            /* sum = F + B; carry = (sum - ((F ^ B) & 0x8421)) & 0x8420;                  \
               out = ((sum - carry) | (carry - (carry >> 5))) & 0x7fff */                 \
            "ldrh   r9, [r7]\n\t"                                                         \
            QUARTER                                                                       \
            "movw   lr, #0x8421\n\t"                                                      \
            "ubfx   r9, r9, #0, #15\n\t"                                                  \
            "eor    r10, r11, r9\n\t"                                                     \
            "add    r12, r11, r9\n\t"                                                     \
            "and    r10, r10, lr\n\t"                                                     \
            "sub    r10, r12, r10\n\t"                                                    \
            "sub    lr, lr, #1\n\t"                                                       \
            "and    r10, r10, lr\n\t"                                                     \
            "sub    r12, r12, r10\n\t"                                                    \
            "sub    r10, r10, r10, lsr #5\n\t"                                            \
            "orr    r11, r12, r10\n\t"                                                    \
            "bfc    r11, #15, #17\n\t"                                                    \
            "2:\n\t"                                                                      \
            "strh   r11, [r7]\n\t"                                                        \
            "3:\n\t"                                                                      \
            "adds   r7, r7, #2\n\t"                                                       \
            "lsls   r9, r6, #23\n\t"                                                      \
            "bne    1b\n\t"                                                               \
            "pop    {r4-r11, pc}\n\t");                                                   \
    }

GPU_G4_LOOP_T(gpu_g4_loop_t1, "")
GPU_G4_LOOP_T(gpu_g4_loop_t3, "lsr    r11, r11, #2\n\t" "bic    r11, r11, #0x318\n\t")

#undef GPU_G4_LOOP_T
#else
#define GPU_G4_ASM 0
#endif

__attribute__((always_inline)) static inline void gpu_span_g4(const gpu_span_t *s, uint16_t *dst, int n, uint32_t u,
                                                              uint32_t v, int32_t r, int32_t g, int32_t b,
                                                              const int TRANSP, const int CL)
{
    const uint16_t *const tex = s->tex;
    const uint32_t du = (uint32_t)s->du;
    const uint32_t dv = (uint32_t)s->dv;
    const int transp_mode = s->transp_mode;
    const int32_t dr = s->dr, dg = s->dg, db = s->db;
    uint32_t dither = s->dither;

    while (n > 0)
    {
        const int chunk = (n < 64) ? n : 64;
        uint32_t *const col = &g_gpu_g4_col[2 * (64 - chunk)];

        /* the lines arrive while the colours are worked out */
        gpu_span_prefetch4(tex, dst, chunk, u, v, du, dv, TRANSP);

        for (int i = 0; i < chunk; i++)
        {
            const int32_t d = (int32_t)(int8_t)(dither & 0xffu);

            dither = (dither >> 8) | (dither << 24);

            if (CL)
            {
                col[2 * i] = gpu_span_ch(r, d) | (gpu_span_ch(g, d) << 16);
                col[2 * i + 1] = gpu_span_ch(b, d);
            }
            else
            {
                col[2 * i] = (uint32_t)(uint16_t)((r >> ATTR_FRAC_BITS) + d) | ((uint32_t)((g >> ATTR_FRAC_BITS) + d) << 16);
                col[2 * i + 1] = (uint32_t)((b >> ATTR_FRAC_BITS) + d);
            }

            r += dr;
            g += dg;
            b += db;
        }

#if GPU_G4_ASM
        if (!s->exact && (!TRANSP || (transp_mode == 1) || (transp_mode == 3)))
        {
            const gpu_g4_loop_t p = { u, v, du, dv, tex, &g_gpu_split[0][0], col, dst };

            if (!TRANSP)
                gpu_g4_loop_o(&p);
            else if (transp_mode == 1)
                gpu_g4_loop_t1(&p);
            else
                gpu_g4_loop_t3(&p);

            u += du * (uint32_t)chunk;
            v += dv * (uint32_t)chunk;
            dst += chunk;
            n -= chunk;
            continue;
        }
#endif

        const uint32_t *c = col;
        uint16_t *const end = dst + chunk;

#pragma GCC unroll 1
        do
        {
            const uint32_t idx =
                (tex[((v >> ATTR_FRAC_BITS) & 0xffu) * PSX_GPU_VRAM_PITCH + ((u >> (ATTR_FRAC_BITS + 2)) & 0x3fu)] >>
                 ((u >> (ATTR_FRAC_BITS - 2)) & 0xcu)) & 0xfu;
            const int32_t w0 = (int32_t)g_gpu_split[idx][0];
            const int32_t w1 = (int32_t)g_gpu_split[idx][1];
            const int32_t rg = (int32_t)c[0];
            const int32_t bl = (int32_t)c[1];

            u += du;
            v += dv;
            c += 2;

            if (w1)
            {
                const uint32_t out = GPU_USAT_ASR((int32_t)(int16_t)w0 * (int32_t)(int16_t)rg, 5, 7) |
                                     (GPU_USAT_ASR((w0 >> 16) * (rg >> 16), 5, 7) << 5) |
                                     (GPU_USAT_ASR((w1 >> 16) * (int32_t)(int16_t)bl, 5, 7) << 10);

                if (TRANSP && (w1 & 1))
                    *dst = gpu_blend_bgr555(out, *dst, transp_mode);
                else
                    *dst = (uint16_t)(TRANSP ? out : (out | (((uint32_t)w1 & 1u) << 15)));
            }

            dst++;
        }
        while (dst != end);

        n -= chunk;
    }
}

#define GPU_SPAN_G4_FN(T, CL)                                                                                  \
    static PSX_GPU_RAS __attribute__((noinline)) void gpu_span_g4_##T##CL(                                      \
        const gpu_span_t *s, uint16_t *dst, int n, uint32_t u, uint32_t v, int32_t r, int32_t g, int32_t b)     \
    {                                                                                                          \
        gpu_span_g4(s, dst, n, u, v, r, g, b, T, CL);                                                          \
    }

GPU_SPAN_G4_FN(0, 0) GPU_SPAN_G4_FN(0, 1) GPU_SPAN_G4_FN(1, 0) GPU_SPAN_G4_FN(1, 1)

#undef GPU_SPAN_G4_FN

/* transparency * 2 + clamping */
static gpu_span_fn_t const g_gpu_span_g4_fns[4] = { gpu_span_g4_00, gpu_span_g4_01, gpu_span_g4_10, gpu_span_g4_11 };

#define GPU_SPAN_FN(F, SH, T)                                                                             \
    static PSX_GPU_RAS __attribute__((noinline)) void gpu_span_##F##SH##T(                                 \
        const gpu_span_t *s, uint16_t *dst, int n, uint32_t u, uint32_t v, int32_t r, int32_t g, int32_t b) \
    {                                                                                                      \
        gpu_span_tex(s, dst, n, u, v, r, g, b, F, SH, T);                                                  \
    }

GPU_SPAN_FN(0, 0, 0) GPU_SPAN_FN(0, 0, 1) GPU_SPAN_FN(0, 1, 0) GPU_SPAN_FN(0, 1, 1)
GPU_SPAN_FN(1, 0, 0) GPU_SPAN_FN(1, 0, 1) GPU_SPAN_FN(1, 1, 0) GPU_SPAN_FN(1, 1, 1) GPU_SPAN_FN(1, 2, 0) GPU_SPAN_FN(1, 2, 1)
GPU_SPAN_FN(2, 0, 0) GPU_SPAN_FN(2, 0, 1) GPU_SPAN_FN(2, 1, 0) GPU_SPAN_FN(2, 1, 1) GPU_SPAN_FN(2, 2, 0) GPU_SPAN_FN(2, 2, 1)

#undef GPU_SPAN_FN

/* indexed fetch * 6 + shade * 2 + transparency, as the switch of the general loops */
static gpu_span_fn_t const g_gpu_span_fns[18] = {
    gpu_span_000, gpu_span_001, gpu_span_010, gpu_span_011, gpu_span_g4_01, gpu_span_g4_11, /* (4 bit Gouraud: gpu_span_g4) */
    gpu_span_100, gpu_span_101, gpu_span_110, gpu_span_111, gpu_span_120, gpu_span_121,
    gpu_span_200, gpu_span_201, gpu_span_210, gpu_span_211, gpu_span_220, gpu_span_221,
};

/* the four dither values of each row of g_psx_gpu_dither_kernel, as signed bytes, first one lowest */
static const uint32_t g_gpu_dither_rows[4] = { 0x01fd00fcu, 0xff03fe02u, 0x00fc01fdu, 0xfe02ff03u };

static inline uint32_t gpu_dither_row_word(uint32_t row)
{
    return g_gpu_dither_rows[row & 3u];
}

/*
    Gouraud shading without a texture, opaque. A channel is clamp((v >> 12) + d, 0, 255) >> 3
    (PSXE_CH, then 5 bits), which is (v + d * 4096) >> 15 saturated to 0 .. 31: one ADD and one
    USAT. Four pixels a round, so that the row's four dither values (s->dither, from the span's
    first pixel on) stay in registers, already in the colour's 8.12 scale.
*/
#if GPU_G4_ASM
typedef struct
{
    int32_t r, g, b, dr, dg, db;
    uint32_t dither;
    uint16_t *dst;
    int n;
} gpu_gouraud_loop_t;

/* gpu_span_gouraud_o's loop by hand (GCC merged the stores into words with BFIs and ran out of
   registers): r0-r2 colours, r3-r5 steps, r6-r9 the four dither values, r10 destination, r11 count */
static PSX_GPU_RAS __attribute__((naked, noinline)) void gpu_gouraud_loop_o(const gpu_gouraud_loop_t *p)
{
    (void)p;

#define GPU_GOURAUD_PX_ASM(D)                  \
        "add    r12, r0, " D "\n\t"            \
        "add    lr, r1, " D "\n\t"             \
        "usat   r12, #5, r12, asr #15\n\t"     \
        "usat   lr, #5, lr, asr #15\n\t"       \
        "orr    r12, r12, lr, lsl #5\n\t"      \
        "add    lr, r2, " D "\n\t"             \
        "usat   lr, #5, lr, asr #15\n\t"       \
        "add    r0, r0, r3\n\t"                \
        "orr    r12, r12, lr, lsl #10\n\t"     \
        "add    r1, r1, r4\n\t"                \
        "strh   r12, [r10], #2\n\t"            \
        "add    r2, r2, r5\n\t"

    __asm__ volatile(
        "push   {r4-r11, lr}\n\t"
        "ldm    r0, {r0-r8}\n\t"
        "mov    r10, r7\n\t"
        "mov    r11, r8\n\t"
        "sbfx   r7, r6, #8, #8\n\t"
        "sbfx   r8, r6, #16, #8\n\t"
        "asr    r9, r6, #24\n\t"
        "sbfx   r6, r6, #0, #8\n\t"
        "lsl    r7, r7, #12\n\t"
        "lsl    r8, r8, #12\n\t"
        "lsl    r9, r9, #12\n\t"
        "lsl    r6, r6, #12\n\t"
        "subs   r11, r11, #4\n\t"
        "blt    2f\n\t"
        "1:\n\t"
        GPU_GOURAUD_PX_ASM("r6")
        GPU_GOURAUD_PX_ASM("r7")
        GPU_GOURAUD_PX_ASM("r8")
        GPU_GOURAUD_PX_ASM("r9")
        "subs   r11, r11, #4\n\t"
        "bge    1b\n\t"
        "2:\n\t"
        "adds   r11, r11, #4\n\t"
        "beq    3f\n\t"
        GPU_GOURAUD_PX_ASM("r6")
        "subs   r11, r11, #1\n\t"
        "beq    3f\n\t"
        GPU_GOURAUD_PX_ASM("r7")
        "subs   r11, r11, #1\n\t"
        "beq    3f\n\t"
        GPU_GOURAUD_PX_ASM("r8")
        "3:\n\t"
        "pop    {r4-r11, pc}\n\t");

#undef GPU_GOURAUD_PX_ASM
}
#endif

static PSX_GPU_RAS __attribute__((noinline)) void gpu_span_gouraud_o(const gpu_span_t *s, uint16_t *dst, int n,
                                                                      int32_t r, int32_t g, int32_t b)
{
#if GPU_G4_ASM
    const gpu_gouraud_loop_t p = { r, g, b, s->dr, s->dg, s->db, s->dither, dst, n };

    gpu_gouraud_loop_o(&p);
#else
    const int32_t dr = s->dr, dg = s->dg, db = s->db;
    const uint32_t w = s->dither;
    const int32_t d0 = (int32_t)(int8_t)w * (1 << ATTR_FRAC_BITS);
    const int32_t d1 = (int32_t)(int8_t)(w >> 8) * (1 << ATTR_FRAC_BITS);
    const int32_t d2 = (int32_t)(int8_t)(w >> 16) * (1 << ATTR_FRAC_BITS);
    const int32_t d3 = (int32_t)(int8_t)(w >> 24) * (1 << ATTR_FRAC_BITS);

#define GPU_GOURAUD_PX(D)                                                                              \
    do                                                                                                 \
    {                                                                                                  \
        *dst++ = (uint16_t)(GPU_USAT_ASR(r + (D), 5, 15) | (GPU_USAT_ASR(g + (D), 5, 15) << 5) |        \
                            (GPU_USAT_ASR(b + (D), 5, 15) << 10));                                      \
        r += dr;                                                                                       \
        g += dg;                                                                                       \
        b += db;                                                                                       \
    } while (0)

#pragma GCC unroll 1
    for (; n >= 4; n -= 4)
    {
        GPU_GOURAUD_PX(d0);
        GPU_GOURAUD_PX(d1);
        GPU_GOURAUD_PX(d2);
        GPU_GOURAUD_PX(d3);
    }

    if (n > 0)
    {
        GPU_GOURAUD_PX(d0);

        if (n > 1)
        {
            GPU_GOURAUD_PX(d1);

            if (n > 2)
                GPU_GOURAUD_PX(d2);
        }
    }

#undef GPU_GOURAUD_PX
#endif
}

/*
    Gouraud shading with one colour.

    A Gouraud shaded polygon whose three vertices have the same colour is shaded
    in that colour everywhere - the planes are flat, every pixel's colour is the
    vertex colour plus its dither offset, clamped. Games draw most of their
    lit geometry that way (Disney's Atlantis: 699 of 746 textured Gouraud
    triangles, and all of the big semi transparent ones behind its menu), and
    the general loops still interpolate three channels, dither and clamp them
    and multiply every texel by them, pixel after pixel.

    With a 4 bit texture a pixel can only come out as one of 16 palette
    entries modulated by one of the 16 dithered colours of the 4x4 kernel. So
    those 256 values are made once per polygon - and kept for the next one when
    colour, transparency and palette are the same, which along a lit mesh they
    mostly are - and a pixel is a texel fetch and a table lookup. The values
    are the ones the general loop makes, bit for bit (gpu_mod_px of the texel
    by clamp(colour + dither)).

    An entry: the pixel as it goes into VRAM, bit 16 "drawn" (the texel is not
    0), bit 17 "blended" (semi transparent primitive, texel mask bit set).
    Laid out [kernel row][texel][kernel column], so that a span of one row
    reads a single 256 byte block - and only kernel rows 0 and 1 are made:
    rows 2 and 3 are rows 0 and 1 turned by two columns (-4 0 -3 1 / -3 1 -4 0,
    2 -2 3 -1 / 3 -1 2 -2), so a span of row 2 or 3 reads row 0 or 1 starting
    two columns on. The 8 (row, column) cells of rows 0 and 1 hold the 8 dither
    values -4 .. 3 once each: 128 entries, each made once, where it is read.

    Flat lit models change colour from polygon to polygon (the table is made
    for each of them), so the entries are made from the palette words of
    gpu_split_make - three products and three USATs, gpu_mod_px bit for bit.

    Blending mode 3 (B + F / 4): its blended entries hold F / 4 already, the
    quarter gpu_blend_bgr555 takes, so that mode 1's span (B + F) draws it. An
    opaque polygon's entries are never "blended", so mode 1's span draws those
    too: one loop for all three (by hand on the M7, gpu_span_mtab_t1).

    Making the table costs about as much as drawing 200 of its pixels, and most
    such polygons are small (Atlantis in game: 918 of 1433 draw under 16 pixels,
    299 of the 536 tables were made for one of those). A small polygon whose
    table is not there already is left to the Gouraud span loops instead, which
    draw the same pixels (build 0 below: nothing made, the old table kept) -
    unless the small polygon before it wanted the same table: small polygons
    come in runs of one colour, and a run gets its table on the second one.
*/
static uint32_t PSX_GPU_DTCM_BSS __attribute__((aligned(8))) g_gpu_mtab[2][16][4];
static uint32_t PSX_GPU_DTCM_BSS g_gpu_mtab_key[9]; /* colour | transparency | valid, then the 16 palette entries */
static uint32_t PSX_GPU_DTCM_BSS g_gpu_mtab_want[9]; /* the same of the last table left unmade */

/* polygons with twice their area (gpu_render_triangle_impl's `area`) under this get a table only
   when it is already made */
#ifndef GPU_MTAB_MIN_AREA
#define GPU_MTAB_MIN_AREA 128
#endif

/* 1: g_gpu_mtab is the table of this palette, colour and blending; 0: it is not, and `build` was 0 */
static PSX_GPU_RAS __attribute__((noinline)) int gpu_mtab_make(const uint16_t *clut, uint32_t color, int transp,
                                                                int mode, int build)
{
    const uint32_t key = (color & 0xffffffu) | (transp ? (0x1000000u | ((uint32_t)mode << 26)) : 0u) | 0x2000000u;

    uint32_t pal[8];

    gpu_pal_load(pal, clut);

    if ((g_gpu_mtab_key[0] == key) && gpu_pal_same(&g_gpu_mtab_key[1], pal))
        return 1;

    if (!build && ((g_gpu_mtab_want[0] != key) || !gpu_pal_same(&g_gpu_mtab_want[1], pal)))
    {
        g_gpu_mtab_want[0] = key;
        gpu_pal_load(&g_gpu_mtab_want[1], clut);
        return 0;
    }

    g_gpu_mtab_key[0] = key;
    gpu_pal_load(&g_gpu_mtab_key[1], clut);

    gpu_split_make(clut);

    /* per texel what goes on top of the colour: drawn, its mask bit, blended - or 0, not drawn */
    uint32_t fl[16];

    for (uint32_t i = 0; i < 16u; i++)
    {
        const uint32_t w1 = g_gpu_split[i][1];

        fl[i] = w1 ? (0x10000u | ((w1 & 1u) << 15) | ((transp && (w1 & 1u)) ? 0x20000u : 0u)) : 0u;
    }

    const int32_t r = (int32_t)(color & 0xffu), g = (int32_t)((color >> 8) & 0xffu), b = (int32_t)((color >> 16) & 0xffu);

    for (uint32_t ry = 0; ry < 2u; ry++)
        for (uint32_t cx = 0; cx < 4u; cx++)
        {
            const int32_t d = g_psx_gpu_dither_kernel[(ry << 2) | cx];
            const int32_t mrg = (int32_t)(gpu_span_ch(r << ATTR_FRAC_BITS, d) | (gpu_span_ch(g << ATTR_FRAC_BITS, d) << 16));
            const int32_t mb = (int32_t)gpu_span_ch(b << ATTR_FRAC_BITS, d);

            for (uint32_t i = 0; i < 16u; i++)
            {
                const int32_t w0 = (int32_t)g_gpu_split[i][0];
                const int32_t w1 = (int32_t)g_gpu_split[i][1];
                const uint32_t px = GPU_USAT_ASR((int32_t)(int16_t)w0 * (int32_t)(int16_t)mrg, 5, 7) |
                                    (GPU_USAT_ASR((w0 >> 16) * (mrg >> 16), 5, 7) << 5) |
                                    (GPU_USAT_ASR((w1 >> 16) * mb, 5, 7) << 10);

                const uint32_t f = (transp && (mode == 3) && (fl[i] & 0x20000u)) ? ((px >> 2) & 0x1ce7u) : px;

                g_gpu_mtab[ry][i][cx] = fl[i] ? (f | fl[i]) : 0u;
            }
        }

    return 1;
}

/* One span of such a polygon with a 4 bit texture: `tab` is its kernel row's block, `col` the kernel
   column of its first pixel. TRANSP and MODE are constants: the blend is inlined for its mode. */
__attribute__((always_inline)) static inline void gpu_span_mtab(const uint32_t *tab, const uint16_t *tex,
                                                                 uint16_t *dst, int n, uint32_t col, uint32_t u,
                                                                 uint32_t v, uint32_t du, uint32_t dv,
                                                                 const int TRANSP, const int MODE)
{
#pragma GCC unroll 1
    do
    {
        const uint32_t idx =
            (tex[((v >> ATTR_FRAC_BITS) & 0xffu) * PSX_GPU_VRAM_PITCH + ((u >> (ATTR_FRAC_BITS + 2)) & 0x3fu)] >>
             ((u >> (ATTR_FRAC_BITS - 2)) & 0xcu)) & 0xfu;
        const uint32_t e = tab[(idx << 2) | col];

        col = (col + 1u) & 3u;
        u += du;
        v += dv;

        if (e & 0x10000u)
            *dst = (uint16_t)((TRANSP && (e & 0x20000u)) ? gpu_blend_bgr555(e, *dst, MODE) : e);

        dst++;
    }
    while (--n > 0);
}

typedef void (*gpu_span_mtab_fn_t)(const uint32_t *tab, const uint16_t *tex, uint16_t *dst, int n, uint32_t col,
                                   uint32_t u, uint32_t v, uint32_t du, uint32_t dv);

#define GPU_SPAN_MTAB_FN(NAME, T, M)                                                                          \
    static PSX_GPU_RAS __attribute__((noinline)) void NAME(const uint32_t *tab, const uint16_t *tex, uint16_t *dst, \
                                                          int n, uint32_t col, uint32_t u, uint32_t v, uint32_t du, \
                                                          uint32_t dv)                                          \
    {                                                                                                          \
        gpu_span_mtab(tab, tex, dst, n, col, u, v, du, dv, T, M);                                              \
    }

GPU_SPAN_MTAB_FN(gpu_span_mtab_t0, 1, 0)
GPU_SPAN_MTAB_FN(gpu_span_mtab_t2, 1, 2)

#if GPU_G4_ASM
/*
    Blending mode 1 (B + F), the kind Atlantis draws its light and glow with - three quarters of
    those texels are transparent - and with it opaque polygons and mode 3 (see gpu_mtab_make),
    scheduled by hand like gpu_g4_loop_o: the next pixel's texel is loaded a pixel ahead. The
    arithmetic is gpu_span_mtab's with gpu_blend_bgr555's mode 1.
    Registers: r0 table row, r1 texture page, r2 destination, r3 count, r4 kernel column,
    r5 u, r6 v, r7 du, r8 dv, r9 the texel word ahead, r10-r12 / lr work.
*/
static PSX_GPU_RAS __attribute__((naked, noinline)) void gpu_span_mtab_t1(const uint32_t *tab, const uint16_t *tex,
                                                                          uint16_t *dst, int n, uint32_t col,
                                                                          uint32_t u, uint32_t v, uint32_t du,
                                                                          uint32_t dv)
{
    (void)tab, (void)tex, (void)dst, (void)n, (void)col, (void)u, (void)v, (void)du, (void)dv;

    __asm__ volatile(
        "push   {r4-r11, lr}\n\t"
        "ldrd   r4, r5, [sp, #36]\n\t"
        "ldrd   r6, r7, [sp, #44]\n\t"
        "ldr    r8, [sp, #52]\n\t"
        "ubfx   r10, r6, #12, #8\n\t"
        "ubfx   r9, r5, #14, #6\n\t"
        "add    r10, r10, r10, lsl #6\n\t"
        "add    r9, r9, r10, lsl #4\n\t"
        "ldrh   r9, [r1, r9, lsl #1]\n\t"
        "1:\n\t"
        /* this pixel's table entry; the next pixel's coordinates and texel word */
        "lsr    r10, r5, #10\n\t"
        "and    r10, r10, #12\n\t"
        "lsr    r10, r9, r10\n\t"
        "add    r5, r5, r7\n\t"
        "and    r10, r10, #15\n\t"
        "add    r6, r6, r8\n\t"
        "orr    r10, r4, r10, lsl #2\n\t"
        "ubfx   r11, r6, #12, #8\n\t"
        "ldr    r12, [r0, r10, lsl #2]\n\t"
        "ubfx   r9, r5, #14, #6\n\t"
        "add    r11, r11, r11, lsl #6\n\t"
        "add    r4, r4, #1\n\t"
        "add    r9, r9, r11, lsl #4\n\t"
        "and    r4, r4, #3\n\t"
        "ldrh   r9, [r1, r9, lsl #1]\n\t"
        "tst    r12, #0x10000\n\t"
        "beq    3f\n\t"
        "tst    r12, #0x20000\n\t"
        "beq    2f\n\t"
        /* sum = F + B; carry = (sum - ((F ^ B) & 0x8421)) & 0x8420;
           out = ((sum - carry) | (carry - (carry >> 5))) & 0x7fff */
        "ldrh   r10, [r2]\n\t"
        "ubfx   r12, r12, #0, #15\n\t"
        "movw   lr, #0x8421\n\t"
        "ubfx   r10, r10, #0, #15\n\t"
        "eor    r11, r12, r10\n\t"
        "add    r12, r12, r10\n\t"
        "and    r11, r11, lr\n\t"
        "sub    r11, r12, r11\n\t"
        "sub    lr, lr, #1\n\t"
        "and    r11, r11, lr\n\t"
        "sub    r12, r12, r11\n\t"
        "sub    r11, r11, r11, lsr #5\n\t"
        "orr    r12, r12, r11\n\t"
        "bfc    r12, #15, #17\n\t"
        "2:\n\t"
        "strh   r12, [r2]\n\t"
        "3:\n\t"
        "adds   r2, r2, #2\n\t"
        "subs   r3, r3, #1\n\t"
        "bne    1b\n\t"
        "pop    {r4-r11, pc}\n\t");
}

/* the same in C, reading in order, for a polygon over its own texture page (page_hit) */
GPU_SPAN_MTAB_FN(gpu_span_mtab_c1, 1, 1)
#else
GPU_SPAN_MTAB_FN(gpu_span_mtab_t1, 1, 1)
#define gpu_span_mtab_c1 gpu_span_mtab_t1
#endif

#undef GPU_SPAN_MTAB_FN

/* opaque, then semi transparent by mode (opaque and mode 3 through mode 1's span, see gpu_mtab_make) */
static gpu_span_mtab_fn_t const g_gpu_span_mtab_fns[5] = {
    gpu_span_mtab_t1, gpu_span_mtab_t0, gpu_span_mtab_t1, gpu_span_mtab_t2, gpu_span_mtab_t1,
};
static gpu_span_mtab_fn_t const g_gpu_span_mtab_exact[5] = {
    gpu_span_mtab_c1, gpu_span_mtab_t0, gpu_span_mtab_c1, gpu_span_mtab_t2, gpu_span_mtab_c1,
};

#if PSX_PROFILE
static uint32_t g_ras_last_bbox;
#endif

/* Returns the class of the primitive (see psx_prof_t), for the profiler */
static inline __attribute__((always_inline)) int gpu_render_triangle_impl(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge)
{
    // OPTIMIZATION: Compute only essential flags first, defer expensive computations
    const int is_textured = (data.attrib & PA_TEXTURED) != 0;
    const int is_shaded = (data.attrib & PA_SHADED) != 0;
    const int is_raw = (data.attrib & PA_RAW) != 0;
    const int transparency_enabled = (data.attrib & PA_TRANSP) != 0;

    vertex_t a = v0;
    vertex_t b, c;
    if (EDGE(v0, v1, v2) < 0)
    {
        b = v2;
        c = v1;
    }
    else
    {
        b = v1;
        c = v2;
    }

    const int off_x = gpu->off_x;
    const int off_y = gpu->off_y;
    a.x += off_x;
    b.x += off_x;
    c.x += off_x;
    a.y += off_y;
    b.y += off_y;
    c.y += off_y;

    // OPTIMIZATION: Early exit for degenerate triangles (zero/negative area)
    int64_t area64 = (int64_t)(b.x - a.x) * (int64_t)(c.y - a.y) - (int64_t)(b.y - a.y) * (int64_t)(c.x - a.x);
    if (area64 <= 0)
        return 0;
    int32_t area = (int32_t)area64;

    int xmin = max(min3(a.x, b.x, c.x), gpu->draw_x1);
    int ymin = max(min3(a.y, b.y, c.y), gpu->draw_ry1);
    int xmax = min(max3(a.x, b.x, c.x), min(gpu->draw_x2, 1023));
    int ymax = min(max3(a.y, b.y, c.y), min(gpu->draw_ry2, 511));

    if (xmin > xmax || ymin > ymax)
        return 0;

    gpu_prim_rows(gpu, ymin, ymax + 1);

    /* the field on screen stays as it is (gpu_update_field): every other row,
       from the first one of the other field */
    const int ystep = (gpu->skip_rows >= 0) ? 2 : 1;
    const int yfirst = ((ymin & 1) == gpu->skip_rows) ? (ymin + 1) : ymin;
    const int row_stride = PSX_GPU_VRAM_PITCH * ystep;

    if (yfirst > ymax)
        return 0;

#if PSX_PROFILE
    {
        const uint32_t bbox = (uint32_t)(xmax - xmin + 1) * (uint32_t)(ymax - ymin + 1);
        const int prof_depth = (data.texp >> 7) & 3;

        g_ras_last_bbox = bbox;

        g_prof.pixels += bbox;

        if (!is_textured)
        {
            if (is_shaded)
                g_prof.px_shade += bbox;
            else
                g_prof.px_flat += bbox;
        }
        else if (prof_depth == 0)
            g_prof.px_t4 += bbox;
        else if (prof_depth == 1)
            g_prof.px_t8 += bbox;
        else
            g_prof.px_t15 += bbox;
    }
#endif

    edge_func_t edge0 = edge_setup(a, b);
    edge_func_t edge1 = edge_setup(b, c);
    edge_func_t edge2 = edge_setup(c, a);

    int32_t e0_row = edge_eval(&edge0, xmin, yfirst);
    int32_t e1_row = edge_eval(&edge1, xmin, yfirst);
    int32_t e2_row = edge_eval(&edge2, xmin, yfirst);

    uint16_t *vram = gpu->vram;
    const uint32_t flat_color = data.v[0].c;
    const uint16_t flat_color555 = rgb888_to_bgr555(flat_color);
    const int32_t edge0_a = edge0.a;
    const int32_t edge1_a = edge1.a;
    const int32_t edge2_a = edge2.a;
    const int32_t edge0_b = edge0.b * ystep;
    const int32_t edge1_b = edge1.b * ystep;
    const int32_t edge2_b = edge2.b * ystep;

    // ==== FAST PATH: Flat untextured non-transparent ====
    // Matches original implementation exactly for best compatibility
    if (!is_textured && !is_shaded && !transparency_enabled)
    {
        const uint16_t out_color = flat_color555;
        const int row_px = xmax - xmin + 1;
        int vram_row = yfirst * PSX_GPU_VRAM_PITCH;

        for (int y = yfirst; y <= ymax; y += ystep, vram_row += row_stride)
        {
            /* Solve for the span instead of walking it: a flat fill then costs
               three divisions per scanline plus the stores, rather than three
               edge updates per pixel. */
            int start = 0;
            int end = row_px - 1;

            gpu_span_clip(e0_row, edge0_a, &start, &end);
            gpu_span_clip(e1_row, edge1_a, &start, &end);
            gpu_span_clip(e2_row, edge2_a, &start, &end);

            if (start <= end)
                gpu_fill_span(&vram[vram_row + xmin + start], out_color, end - start + 1);

            e0_row += edge0_b;
            e1_row += edge1_b;
            e2_row += edge2_b;
        }
        return 1;
    }

    // ==== FAST PATH: Flat untextured (transparent or needs blending) ====
    if (!is_textured && !is_shaded)
    {
        const int transp_mode = (gpu->gpustat >> 5) & 3;
        const uint16_t src_color = flat_color555;

        int vram_row = yfirst * PSX_GPU_VRAM_PITCH;

        const int row_px = xmax - xmin + 1;

        for (int y = yfirst; y <= ymax; y += ystep, vram_row += row_stride)
        {
            /* Same solved span as the opaque path, so the blend loop no longer
               carries the edge functions. */
            int start = 0;
            int end = row_px - 1;

            gpu_span_clip(e0_row, edge0_a, &start, &end);
            gpu_span_clip(e1_row, edge1_a, &start, &end);
            gpu_span_clip(e2_row, edge2_a, &start, &end);

            uint16_t *__restrict dst = &vram[vram_row + xmin + start];

            if ((transp_mode != 2) && (start <= end))
            {
                /*
                    Modes 0, 1 and 3 on two pixels at a time, a word of VRAM: their
                    carries never leave a pixel (two 15 bit pixels add up to at most
                    0xfffe, and what is taken off is part of that sum), so the
                    arithmetic of gpu_blend_bgr555 works on both halves at once.
                    (Shadows: Atlantis draws them this way, mode 0.)
                */
                const uint32_t f = (transp_mode == 3) ? ((src_color & 0x7fffu) >> 2) & 0x1ce7u : (src_color & 0x7fffu);
                const uint32_t f2 = f | (f << 16);
                int n = end - start + 1;

                if ((uintptr_t)dst & 2u)
                {
                    *dst = gpu_blend_bgr555(src_color, *dst, transp_mode);
                    dst++;
                    n--;
                }

                uint32_t *d2 = (uint32_t *)dst;

                if (transp_mode == 0)
                {
                    for (; n >= 2; n -= 2, d2++)
                    {
                        const uint32_t b2 = *d2 & 0x7fff7fffu;

                        *d2 = ((f2 + b2) - ((f2 ^ b2) & 0x04210421u)) >> 1;
                    }
                }
                else
                {
                    for (; n >= 2; n -= 2, d2++)
                    {
                        const uint32_t b2 = *d2 & 0x7fff7fffu;
                        const uint32_t sum = f2 + b2;
                        const uint32_t carry = (sum - ((f2 ^ b2) & 0x84218421u)) & 0x84208420u;

                        *d2 = ((sum - carry) | (carry - (carry >> 5))) & 0x7fff7fffu;
                    }
                }

                if (n)
                {
                    dst = (uint16_t *)d2;
                    *dst = gpu_blend_bgr555(src_color, *dst, transp_mode);
                }
            }
            else
            {
                for (int x = start; x <= end; ++x, ++dst)
                    *dst = gpu_blend_bgr555(src_color, *dst, transp_mode);
            }

            e0_row += edge0_b;
            e1_row += edge1_b;
            e2_row += edge2_b;
        }
        return 2;
    }

    plane_attr_t r_plane = {0}, g_plane = {0}, b_plane = {0};
    plane_attr_t tx_plane = {0}, ty_plane = {0};
    int32_t r_row = 0, g_row = 0, b_row = 0;
    int32_t tx_row = 0, ty_row = 0;

    if (is_shaded)
    {
        r_plane = plane_setup(&a, &b, &c, area, (a.c >> 0) & 0xff, (b.c >> 0) & 0xff, (c.c >> 0) & 0xff, ATTR_FRAC_BITS, xmin, yfirst);
        g_plane = plane_setup(&a, &b, &c, area, (a.c >> 8) & 0xff, (b.c >> 8) & 0xff, (c.c >> 8) & 0xff, ATTR_FRAC_BITS, xmin, yfirst);
        b_plane = plane_setup(&a, &b, &c, area, (a.c >> 16) & 0xff, (b.c >> 16) & 0xff, (c.c >> 16) & 0xff, ATTR_FRAC_BITS, xmin, yfirst);
        r_row = r_plane.row;
        g_row = g_plane.row;
        b_row = b_plane.row;
    }

    if (is_textured)
    {
        tx_plane = plane_setup(&a, &b, &c, area, a.tx, b.tx, c.tx, ATTR_FRAC_BITS, xmin, yfirst);
        ty_plane = plane_setup(&a, &b, &c, area, a.ty, b.ty, c.ty, ATTR_FRAC_BITS, xmin, yfirst);
        tx_row = tx_plane.row;
        ty_row = ty_plane.row;
    }

    /*
        Everything from here on interpolates something across the triangle, and
        all of it works the same way:

          - the span of a scanline is solved from the edge functions
            (gpu_span_clip) instead of walked pixel by pixel from the left edge
            of the bounding box. The old loops evaluated three edge functions
            for every pixel of the box, inside the triangle or not - for the
            thin triangles of a character model most of the box is outside;
          - what does not change along a span is decided outside the loop: how
            the texel is fetched (4 / 8 / 15 bit), how it is shaded (as is,
            modulated by one colour, modulated by the interpolated colour) and
            whether it is blended. Each combination gets a loop of its own with
            only the per pixel work left in it.

        The pixels are the same as before, bit for bit; checked on the host
        against the loops this replaces.
    */
    const int row_px = xmax - xmin + 1;
    int vram_row = yfirst * PSX_GPU_VRAM_PITCH;

    const int32_t r_dx = r_plane.dx, g_dx = g_plane.dx, b_dx = b_plane.dx;
    const int32_t r_dy = r_plane.dy * ystep, g_dy = g_plane.dy * ystep, b_dy = b_plane.dy * ystep;

/* a colour channel: 8.12 fixed point plus the dither offset, clamped to 8 bits */
#define PSXE_CH(v, d) ((uint32_t)fast_saturate_u8(((v) >> ATTR_FRAC_BITS) + (d)))

    // ==== Gouraud shading without texture ====
    if (!is_textured)
    {
        const int transp_mode = (gpu->gpustat >> 5) & 3;

#define PSXE_GOURAUD_LOOP(TRANSP)                                                                      \
        for (int y = yfirst; y <= ymax; y += ystep, vram_row += row_stride)                            \
        {                                                                                              \
            int start = 0;                                                                             \
            int end = row_px - 1;                                                                      \
                                                                                                       \
            gpu_span_clip(e0_row, edge0_a, &start, &end);                                              \
            gpu_span_clip(e1_row, edge1_a, &start, &end);                                              \
            gpu_span_clip(e2_row, edge2_a, &start, &end);                                              \
                                                                                                       \
            if (start <= end)                                                                          \
            {                                                                                          \
                /* the dither pattern is anchored at the corner of the bounding box: the row's          \
                   four values as signed bytes, turned so that the span's first pixel is lowest */     \
                const uint32_t dw = gpu_dither_row_word((uint32_t)(y - ymin));                         \
                const uint32_t drot = ((uint32_t)start & 3u) << 3;                                     \
                uint32_t dither = drot ? ((dw >> drot) | (dw << (32u - drot))) : dw;                   \
                                                                                                       \
                int32_t rv = (int32_t)((uint32_t)r_row + (uint32_t)r_dx * (uint32_t)start);            \
                int32_t gv = (int32_t)((uint32_t)g_row + (uint32_t)g_dx * (uint32_t)start);            \
                int32_t bv = (int32_t)((uint32_t)b_row + (uint32_t)b_dx * (uint32_t)start);            \
                uint16_t *__restrict dst = &vram[vram_row + xmin + start];                             \
                uint16_t *const dend = dst + (end - start) + 1;                                        \
                                                                                                       \
                /* a channel is clamp((v >> 12) + d, 0, 255) >> 3, which is                             \
                   clamp(((v >> 12) + d) >> 3, 0, 31): one add and one USAT */                          \
                do                                                                                     \
                {                                                                                      \
                    const int32_t d = (int32_t)(int8_t)(dither & 0xffu);                               \
                                                                                                       \
                    dither = (dither >> 8) | (dither << 24);                                           \
                                                                                                       \
                    const uint32_t out = GPU_USAT_ASR((rv >> ATTR_FRAC_BITS) + d, 5, 3) |              \
                                         (GPU_USAT_ASR((gv >> ATTR_FRAC_BITS) + d, 5, 3) << 5) |       \
                                         (GPU_USAT_ASR((bv >> ATTR_FRAC_BITS) + d, 5, 3) << 10);       \
                                                                                                       \
                    *dst = (uint16_t)((TRANSP) ? gpu_blend_bgr555(out, *dst, transp_mode) : out);       \
                                                                                                       \
                    rv += r_dx; gv += g_dx; bv += b_dx;                                                \
                }                                                                                      \
                while (++dst != dend);                                                                 \
            }                                                                                          \
                                                                                                       \
            e0_row += edge0_b; e1_row += edge1_b; e2_row += edge2_b;                                   \
            r_row += r_dy; g_row += g_dy; b_row += b_dy;                                               \
        }

/* In one colour (all three vertices alike, see gpu_mtab_make) a pixel is one of the 16 dithered
   colours of the kernel - pat[kernel row][kernel column], made once - and TRANSP / MODE are constants */
#define PSXE_GOURAUD_ONE_LOOP(TRANSP, MODE)                                                            \
        for (int y = yfirst; y <= ymax; y += ystep, vram_row += row_stride)                            \
        {                                                                                              \
            int start = 0;                                                                             \
            int end = row_px - 1;                                                                      \
                                                                                                       \
            gpu_span_clip(e0_row, edge0_a, &start, &end);                                              \
            gpu_span_clip(e1_row, edge1_a, &start, &end);                                              \
            gpu_span_clip(e2_row, edge2_a, &start, &end);                                              \
                                                                                                       \
            if (start <= end)                                                                          \
            {                                                                                          \
                const uint16_t *const p = pat[(uint32_t)(y - ymin) & 3u];                              \
                uint32_t k = (uint32_t)start & 3u;                                                     \
                uint16_t *__restrict dst = &vram[vram_row + xmin + start];                             \
                uint16_t *const dend = dst + (end - start) + 1;                                        \
                                                                                                       \
                do                                                                                     \
                {                                                                                      \
                    *dst = (TRANSP) ? gpu_blend_bgr555(p[k], *dst, (MODE)) : p[k];                     \
                    k = (k + 1u) & 3u;                                                                 \
                }                                                                                      \
                while (++dst != dend);                                                                 \
            }                                                                                          \
                                                                                                       \
            e0_row += edge0_b; e1_row += edge1_b; e2_row += edge2_b;                                   \
        }

        if ((a.c == b.c) && (b.c == c.c))
        {
            const int32_t cr = (int32_t)(a.c & 0xffu), cg = (int32_t)((a.c >> 8) & 0xffu),
                          cb = (int32_t)((a.c >> 16) & 0xffu);

            uint16_t pat[4][4];

            for (uint32_t i = 0; i < 16u; i++)
            {
                const int32_t d = g_psx_gpu_dither_kernel[i];

                pat[i >> 2][i & 3u] = (uint16_t)(GPU_USAT_ASR(cr + d, 5, 3) | (GPU_USAT_ASR(cg + d, 5, 3) << 5) |
                                                 (GPU_USAT_ASR(cb + d, 5, 3) << 10));
            }

            if (!transparency_enabled)
            {
                PSXE_GOURAUD_ONE_LOOP(0, 0)
            }
            else
            {
                switch (transp_mode)
                {
                case 0:
                    PSXE_GOURAUD_ONE_LOOP(1, 0)
                    break;
                case 1:
                    PSXE_GOURAUD_ONE_LOOP(1, 1)
                    break;
                case 2:
                    PSXE_GOURAUD_ONE_LOOP(1, 2)
                    break;
                default:
                    PSXE_GOURAUD_ONE_LOOP(1, 3)
                    break;
                }
            }
        }
        else if (transparency_enabled)
        {
            PSXE_GOURAUD_LOOP(1)
        }
        else
        {
            gpu_span_t sp;

            sp.dr = r_dx;
            sp.dg = g_dx;
            sp.db = b_dx;

            for (int y = yfirst; y <= ymax; y += ystep, vram_row += row_stride)
            {
                int start = 0;
                int end = row_px - 1;

                gpu_span_clip(e0_row, edge0_a, &start, &end);
                gpu_span_clip(e1_row, edge1_a, &start, &end);
                gpu_span_clip(e2_row, edge2_a, &start, &end);

                if (start <= end)
                {
                    /* the dither pattern is anchored at the corner of the bounding box: the row's
                       four values as signed bytes, turned so that the span's first pixel is lowest */
                    const uint32_t dw = gpu_dither_row_word((uint32_t)(y - ymin));
                    const uint32_t drot = ((uint32_t)start & 3u) << 3;

                    sp.dither = drot ? ((dw >> drot) | (dw << (32u - drot))) : dw;

                    gpu_span_gouraud_o(&sp, &vram[vram_row + xmin + start], end - start + 1,
                                       (int32_t)((uint32_t)r_row + (uint32_t)r_dx * (uint32_t)start),
                                       (int32_t)((uint32_t)g_row + (uint32_t)g_dx * (uint32_t)start),
                                       (int32_t)((uint32_t)b_row + (uint32_t)b_dx * (uint32_t)start));
                }

                e0_row += edge0_b;
                e1_row += edge1_b;
                e2_row += edge2_b;
                r_row += r_dy;
                g_row += g_dy;
                b_row += b_dy;
            }
        }

#undef PSXE_GOURAUD_LOOP
#undef PSXE_GOURAUD_ONE_LOOP

        return 5;
    }

    // ==== Textured, flat or Gouraud shaded ====
    const int tpx = (data.texp & 0xf) << 6;
    const int tpy = (data.texp & 0x10) << 4;
    const int clutx = (data.clut & 0x3f) << 4;
    const int cluty = (data.clut >> 6) & 0x1ff;
    const int depth = (data.texp >> 7) & 3;
    const int transp_mode = (data.texp >> 5) & 3;

    const uint8_t mod_r = flat_color & 0xff;
    const uint8_t mod_g = (flat_color >> 8) & 0xff;
    const uint8_t mod_b = (flat_color >> 16) & 0xff;

    /* texture window, folded into one and / or pair per coordinate */
    const uint32_t texw_and_x = (uint32_t)(uint16_t)(~gpu->texw_mx) & 0xffu;
    const uint32_t texw_or_x = (uint32_t)(gpu->texw_ox & gpu->texw_mx) & 0xffu;
    const uint32_t texw_and_y = (uint32_t)(uint16_t)(~gpu->texw_my) & 0xffu;
    const uint32_t texw_or_y = (uint32_t)(gpu->texw_oy & gpu->texw_my) & 0xffu;

    /* palette staged in DTCM when the primitive is big enough to profit */
    const uint16_t *const clut = gpu_clut_ptr(gpu, depth, clutx, cluty,
                                              (uint32_t)row_px * (uint32_t)(ymax - ymin + 1));

    const int32_t tx_dx = tx_plane.dx, ty_dx = ty_plane.dx;
    const int32_t tx_dy = tx_plane.dy * ystep, ty_dy = ty_plane.dy * ystep;

    /*
        How the texel is fetched: 0 four bit, 1 eight bit, 2 fifteen bit. The
        reserved depth 3 was read differently by each of the old loops - eight
        bit by the fast flat one, four bit by the raw one, fifteen bit under
        Gouraud shading - and that is kept, odd as it is: nothing should use it,
        and if something does it looks the way it did.
    */
    int fetch;

    if (depth != 3)
        fetch = depth;
    else if (is_shaded)
        fetch = 2;
    else
        fetch = is_raw ? 0 : 1;

    /* How it is shaded: 0 as it is (raw, or modulated by the neutral 0x808080,
       which leaves every five bit channel alone), 1 one colour, 2 interpolated */
    int shade;

    if (is_raw)
        shade = 0;
    else if (is_shaded)
        shade = 2;
    else
        shade = ((mod_r == 0x80) && (mod_g == 0x80) && (mod_b == 0x80)) ? 0 : 1;

#if PSX_PROFILE
    {
        const uint32_t bbox = (uint32_t)row_px * (uint32_t)(ymax - ymin + 1);

        if (transparency_enabled)
            g_prof.px_transp += bbox;

        if (is_raw)
            g_prof.px_raw += bbox;

        g_prof.px_fast += bbox;
    }
#endif

#define PSXE_FETCH(F, tx, ty)                                                                          \
    (((F) == 0)   ? clut[(vram[PSX_VRAM_AT((tpx + ((tx) >> 2)) & 0x3ffu, tpy + (ty))] >> (((tx) & 3u) << 2)) & 0xfu]   \
     : ((F) == 1) ? clut[(vram[PSX_VRAM_AT((tpx + ((tx) >> 1)) & 0x3ffu, tpy + (ty))] >> (((tx) & 1u) << 3)) & 0xffu]  \
                  : vram[PSX_VRAM_AT((tpx + (tx)) & 0x3ffu, tpy + (ty))])

#define PSXE_TEX_LOOP(F, SH, TRANSP)                                                                   \
    for (int y = yfirst; y <= ymax; y += ystep, vram_row += row_stride)                                \
    {                                                                                                  \
        int start = 0;                                                                                 \
        int end = row_px - 1;                                                                          \
                                                                                                       \
        gpu_span_clip(e0_row, edge0_a, &start, &end);                                                  \
        gpu_span_clip(e1_row, edge1_a, &start, &end);                                                  \
        gpu_span_clip(e2_row, edge2_a, &start, &end);                                                  \
                                                                                                       \
        if (start <= end)                                                                              \
        {                                                                                              \
            const int *const drow = &g_psx_gpu_dither_kernel[((y - ymin) & 3) << 2];                   \
                                                                                                       \
            int32_t txv = (int32_t)((uint32_t)tx_row + (uint32_t)tx_dx * (uint32_t)start);             \
            int32_t tyv = (int32_t)((uint32_t)ty_row + (uint32_t)ty_dx * (uint32_t)start);             \
            int32_t rv = (int32_t)((uint32_t)r_row + (uint32_t)r_dx * (uint32_t)start);                \
            int32_t gv = (int32_t)((uint32_t)g_row + (uint32_t)g_dx * (uint32_t)start);                \
            int32_t bv = (int32_t)((uint32_t)b_row + (uint32_t)b_dx * (uint32_t)start);                \
            uint16_t *__restrict dst = &vram[vram_row + xmin + start];                                 \
                                                                                                       \
            /* the texels the next scanline starts with, fetched while this one is drawn: a            \
               texture line that is not in the cache costs about as much as a short span */            \
            {                                                                                          \
                const uint32_t ntx = ((((uint32_t)txv + (uint32_t)tx_dy) >> ATTR_FRAC_BITS) & texw_and_x) | texw_or_x;\
                const uint32_t nty = ((((uint32_t)tyv + (uint32_t)ty_dy) >> ATTR_FRAC_BITS) & texw_and_y) | texw_or_y;\
                                                                                                       \
                __builtin_prefetch(&vram[PSX_VRAM_AT(tpx + (ntx >> (((F) == 0) ? 2 : (((F) == 1) ? 1 : 0))), tpy + nty)]);\
            }                                                                                          \
                                                                                                       \
            for (int x = start; x <= end; ++x, ++dst)                                                  \
            {                                                                                          \
                const uint32_t tx = (((uint32_t)txv >> ATTR_FRAC_BITS) & texw_and_x) | texw_or_x;       \
                const uint32_t ty = (((uint32_t)tyv >> ATTR_FRAC_BITS) & texw_and_y) | texw_or_y;       \
                const uint16_t texel = PSXE_FETCH(F, tx, ty);                                          \
                                                                                                       \
                if (texel)                                                                             \
                {                                                                                      \
                    uint16_t out;                                                                      \
                                                                                                       \
                    if ((SH) == 0)                                                                     \
                    {                                                                                  \
                        out = texel;                                                                   \
                    }                                                                                  \
                    else if ((SH) == 1)                                                                \
                    {                                                                                  \
                        out = modulate_bgr555(texel, mod_r, mod_g, mod_b);                             \
                    }                                                                                  \
                    else                                                                               \
                    {                                                                                  \
                        const int d = drow[x & 3];                                                     \
                                                                                                       \
                        out = modulate_bgr555(texel, (uint8_t)PSXE_CH(rv, d), (uint8_t)PSXE_CH(gv, d), \
                                              (uint8_t)PSXE_CH(bv, d));                                \
                    }                                                                                  \
                                                                                                       \
                    *dst = ((TRANSP) && (texel & 0x8000u)) ? gpu_blend_bgr555(out, *dst, transp_mode)  \
                                                           : out;                                      \
                }                                                                                      \
                                                                                                       \
                txv += tx_dx; tyv += ty_dx;                                                            \
                                                                                                       \
                if ((SH) == 2)                                                                         \
                {                                                                                      \
                    rv += r_dx; gv += g_dx; bv += b_dx;                                                \
                }                                                                                      \
            }                                                                                          \
        }                                                                                              \
                                                                                                       \
        e0_row += edge0_b; e1_row += edge1_b; e2_row += edge2_b;                                       \
        tx_row += tx_dy; ty_row += ty_dy;                                                              \
        r_row += r_dy; g_row += g_dy; b_row += b_dy;                                                   \
    }

    /* A texture page whose 256 texels reach past x = 1023 (8 bit at page x 15,
       15 bit from 13 on) wraps around to the start of its rows; the span
       functions leave that to the general loops, whose fetch wraps. */
    const int page_wraps = (fetch != 0) && ((tpx + (256 >> (2 - fetch))) > 1024);

    /* A polygon that covers part of its own texture page: the hand-written span loops
       (GPU_G4_ASM) load the next pixel's texel before they store this pixel, so a pixel
       stored onto the texel the next one reads would be read too early - such a polygon
       takes the C loops, which read in order. (Games keep what they draw clear of the
       pages they draw with; a synthetic test did not.) */
    const int page_hit = (xmax >= tpx) && (xmin < (tpx + (64 << fetch))) && (ymax >= tpy) && (ymin < (tpy + 256));

    /* Gouraud shaded in one colour, 4 bit texture: the table of its pixels (gpu_mtab_make) - for a
       small polygon only when it is there already */
    if ((shade == 2) && (fetch == 0) && (a.c == b.c) && (b.c == c.c) && !gpu->texw_mx && !gpu->texw_my &&
        gpu_mtab_make(clut, a.c, transparency_enabled, transp_mode, area >= GPU_MTAB_MIN_AREA))
    {
        const gpu_span_mtab_fn_t span =
            (page_hit ? g_gpu_span_mtab_exact : g_gpu_span_mtab_fns)[transparency_enabled ? (1 + transp_mode) : 0];
        const uint16_t *const tex = &vram[PSX_VRAM_AT(tpx, tpy)];

        for (int y = yfirst; y <= ymax; y += ystep, vram_row += row_stride)
        {
            int start = 0;
            int end = row_px - 1;

            gpu_span_clip(e0_row, edge0_a, &start, &end);
            gpu_span_clip(e1_row, edge1_a, &start, &end);
            gpu_span_clip(e2_row, edge2_a, &start, &end);

            if (start <= end)
            {
                const uint32_t txv = (uint32_t)tx_row + (uint32_t)tx_dx * (uint32_t)start;
                const uint32_t tyv = (uint32_t)ty_row + (uint32_t)ty_dx * (uint32_t)start;

                /* the texels the next scanline starts with (as the span functions below) */
                {
                    const uint32_t ntx = ((txv + (uint32_t)tx_dy) >> ATTR_FRAC_BITS) & 0xffu;
                    const uint32_t nty = ((tyv + (uint32_t)ty_dy) >> ATTR_FRAC_BITS) & 0xffu;

                    __builtin_prefetch(&tex[PSX_VRAM_AT(ntx >> 2, nty)]);
                }

                /* kernel rows 2 and 3: rows 0 and 1 from two columns on (gpu_mtab_make) */
                const uint32_t krow = (uint32_t)(y - ymin) & 3u;

                span(&g_gpu_mtab[krow & 1u][0][0], tex, &vram[vram_row + xmin + start], end - start + 1,
                     ((uint32_t)start + (krow & 2u)) & 3u, txv, tyv, (uint32_t)tx_dx, (uint32_t)ty_dx);
            }

            e0_row += edge0_b;
            e1_row += edge1_b;
            e2_row += edge2_b;
            tx_row += tx_dy;
            ty_row += ty_dy;
        }
    }
    else if (!gpu->texw_mx && !gpu->texw_my && !page_wraps)
    {
        /* no texture window: the span functions above */
        gpu_span_t sp;

        sp.tex = &vram[PSX_VRAM_AT(tpx, tpy)];
        sp.clut = clut;
        sp.du = tx_dx;
        sp.dv = ty_dx;
        sp.dr = r_dx;
        sp.dg = g_dx;
        sp.db = b_dx;
        sp.mr = mod_r;
        sp.mg = mod_g;
        sp.mb = mod_b;
        sp.dither = 0;
        sp.transp_mode = transp_mode;
        sp.exact = page_hit;

        const gpu_span_fn_t span = g_gpu_span_fns[(fetch * 6) + (shade * 2) + (transparency_enabled ? 1 : 0)];
        const uint32_t pre_shift = (fetch == 0) ? 2u : ((fetch == 1) ? 1u : 0u);

        /* Gouraud and a 4 bit texture: the palette taken apart (gpu_span_g4) */
        const int g4 = (shade == 2) && (fetch == 0);

        if (g4)
            gpu_split_make(clut);

        for (int y = yfirst; y <= ymax; y += ystep, vram_row += row_stride)
        {
            int start = 0;
            int end = row_px - 1;

            gpu_span_clip(e0_row, edge0_a, &start, &end);
            gpu_span_clip(e1_row, edge1_a, &start, &end);
            gpu_span_clip(e2_row, edge2_a, &start, &end);

            if (start <= end)
            {
                const uint32_t txv = (uint32_t)tx_row + (uint32_t)tx_dx * (uint32_t)start;
                const uint32_t tyv = (uint32_t)ty_row + (uint32_t)ty_dx * (uint32_t)start;

                /* the texels the next scanline starts with, fetched while this one is
                   drawn: a texture line that is not in the cache costs about as much
                   as a short span */
                {
                    const uint32_t ntx = ((txv + (uint32_t)tx_dy) >> ATTR_FRAC_BITS) & 0xffu;
                    const uint32_t nty = ((tyv + (uint32_t)ty_dy) >> ATTR_FRAC_BITS) & 0xffu;

                    __builtin_prefetch(&sp.tex[PSX_VRAM_AT(ntx >> pre_shift, nty)]);
                }

                if (shade == 2)
                {
                    /* the dither pattern is anchored at the corner of the bounding box */
                    const uint32_t w = gpu_dither_row_word((uint32_t)(y - ymin));
                    const uint32_t rot = ((uint32_t)start & 3u) << 3;

                    sp.dither = rot ? ((w >> rot) | (w << (32u - rot))) : w;
                }

                const int32_t rs = (int32_t)((uint32_t)r_row + (uint32_t)r_dx * (uint32_t)start);
                const int32_t gs = (int32_t)((uint32_t)g_row + (uint32_t)g_dx * (uint32_t)start);
                const int32_t bs = (int32_t)((uint32_t)b_row + (uint32_t)b_dx * (uint32_t)start);

                if (g4)
                {
                    /* no clamping when every colour of the span is within 4 .. 251 (its ends) */
                    const uint32_t k = (uint32_t)(end - start);
                    const int32_t re = (int32_t)((uint32_t)rs + (uint32_t)r_dx * k);
                    const int32_t ge = (int32_t)((uint32_t)gs + (uint32_t)g_dx * k);
                    const int32_t be = (int32_t)((uint32_t)bs + (uint32_t)b_dx * k);

#define GPU_IN_4_251(x) (((uint32_t)(((x) >> ATTR_FRAC_BITS) - 4)) <= 247u)
                    const int cl = !(GPU_IN_4_251(rs) && GPU_IN_4_251(gs) && GPU_IN_4_251(bs) && GPU_IN_4_251(re) &&
                                     GPU_IN_4_251(ge) && GPU_IN_4_251(be));
#undef GPU_IN_4_251

                    g_gpu_span_g4_fns[(transparency_enabled ? 2 : 0) + cl](&sp, &vram[vram_row + xmin + start],
                                                                         end - start + 1, txv, tyv, rs, gs, bs);
                }
                else
                {
                    span(&sp, &vram[vram_row + xmin + start], end - start + 1, txv, tyv, rs, gs, bs);
                }
            }

            e0_row += edge0_b;
            e1_row += edge1_b;
            e2_row += edge2_b;
            tx_row += tx_dy;
            ty_row += ty_dy;
            r_row += r_dy;
            g_row += g_dy;
            b_row += b_dy;
        }
    }
    else
    {
        /* A texture window: rare enough for one loop that decides fetch, shading
           and transparency per pixel - the loop the kinds above were expanded
           from, with the same arithmetic. */
        PSXE_TEX_LOOP(fetch, shade, transparency_enabled)
    }

#undef PSXE_TEX_LOOP
#undef PSXE_FETCH
#undef PSXE_CH

    if (!is_shaded)
        return ((depth != 2) && !is_raw) ? 3 : 4;

    return 6;
}


PSX_GPU_RAS void gpu_render_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge)
{
    /* the field nobody sees (gpu_update_hidden) */
    if (gpu->field_guard)
    {
        if (data.attrib & PA_TEXTURED)
            gpu_field_read_tex(gpu, (data.texp & 0xfu) << 6, (data.texp & 0x10u) << 4, (data.texp >> 7) & 3u,
                               data.clut, min3(v0.tx, v1.tx, v2.tx), max3(v0.tx, v1.tx, v2.tx),
                               min3(v0.ty, v1.ty, v2.ty), max3(v0.ty, v1.ty, v2.ty));

        if (gpu->skip_prims)
            return;
    }

#if PSX_GPU_EXTERNAL_RASTER
    if (psx_raster_triangle(gpu, v0, v1, v2, data))
    {
        gpu_touch(gpu, 0, (uint32_t)gpu->draw_y1, PSX_GPU_FB_WIDTH, (uint32_t)gpu->draw_y2 + 1u);

        return;
    }
#endif

#if PSX_PROFILE
    const uint32_t t0 = DWT->CYCCNT;

    g_ras_last_bbox = 0;

    const int k = gpu_render_triangle_impl(gpu, v0, v1, v2, data, edge);

    g_prof.ras_cyc[k] += DWT->CYCCNT - t0;
    g_prof.ras_cnt[k]++;
    g_prof.ras_px[k] += g_ras_last_bbox;
#else
    (void)gpu_render_triangle_impl(gpu, v0, v1, v2, data, edge);
#endif
}

PSX_GPU_RAS void gpu_render_rect(psx_gpu_t *gpu, rect_data_t data)
{
    /* the field nobody sees (gpu_update_hidden) */
    if (gpu->field_guard)
    {
        if (data.attrib & RA_TEXTURED)
        {
            /* texels from (tx, ty) on, as many as the rectangle is big - all of
               the page when that runs past its end */
            const uint32_t u0 = data.v0.tx, v0 = data.v0.ty;
            const uint32_t u1 = (u0 + data.width > 256u) ? 255u : (u0 + data.width - 1u);
            const uint32_t v1 = (v0 + data.height > 256u) ? 255u : (v0 + data.height - 1u);

            if (data.width && data.height)
                gpu_field_read_tex(gpu, gpu->texp_x, gpu->texp_y, gpu->texp_d, data.clut,
                                   (u1 == 255u) ? 0u : u0, u1, (v1 == 255u) ? 0u : v0, v1);
        }

        if (gpu->skip_prims)
            return;
    }

    /* whatever this rectangle writes, the last solid fill (fill_key) is no longer the last word
       on VRAM - unless it is a solid fill that repeats it (below) */
    const uint32_t fill_key = gpu->fill_key;

    gpu->fill_key = 0u;

#if PSX_GPU_EXTERNAL_RASTER
    if (data.width && data.height && psx_raster_rect(gpu, data))
    {
        gpu_touch(gpu, 0, (uint32_t)gpu->draw_y1, PSX_GPU_FB_WIDTH, (uint32_t)gpu->draw_y2 + 1u);

        return;
    }
#endif

    if ((data.v0.x >= 1024) || (data.v0.y >= 512) ||
        (data.v0.x <= -1024) || (data.v0.y <= -512))
        return;

    uint16_t width = data.width;
    uint16_t height = data.height;

    if (!width || !height)
        return;

    const int is_textured = (data.attrib & RA_TEXTURED) != 0;
    const int base_transp = (data.attrib & RA_TRANSP) != 0;
    const int is_raw = (data.attrib & RA_RAW) != 0;
    const int transp_mode = (gpu->gpustat >> 5) & 3;

    const int tpx = gpu->texp_x;
    const int tpy = gpu->texp_y;
    const int clutx = (data.clut & 0x3f) << 4;
    const int cluty = (data.clut >> 6) & 0x1ff;
    const int depth = gpu->texp_d;

    const int32_t screen_x0 = data.v0.x + gpu->off_x;
    const int32_t screen_y0 = data.v0.y + gpu->off_y;
    int32_t x0 = max(screen_x0, gpu->draw_x1);
    int32_t y0 = max(screen_y0, gpu->draw_ry1);
    int32_t x1 = min(screen_x0 + width, gpu->draw_x2 + 1);
    int32_t y1 = min(screen_y0 + height, gpu->draw_ry2 + 1);

    if (x0 >= x1 || y0 >= y1)
        return;

    gpu_prim_rows(gpu, y0, y1);

    /* as a polygon, the field on screen stays as it is (gpu_update_field) */
    const int32_t ystep = (gpu->skip_rows >= 0) ? 2 : 1;
    const int32_t yfirst = ((y0 & 1) == gpu->skip_rows) ? (y0 + 1) : y0;

    if (yfirst >= y1)
        return;

#if PSX_PROFILE
    g_prof.pixels += (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
    g_prof.px_rect += (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
#endif

    const int rect_w = x1 - x0;
    const uint16_t solid_color = rgb888_to_bgr555(data.v0.c);

    if (!is_textured && !base_transp)
    {
        /* inside the last solid fill, in its colour and rows, nothing written since: every
           pixel already is this colour */
        const uint32_t key = 0x80000000u | ((uint32_t)(gpu->skip_rows + 1) << 16) | solid_color;

        if ((fill_key == key) && (x0 >= gpu->fill_x0) && (x1 <= gpu->fill_x1) && (yfirst >= gpu->fill_y0) &&
            (y1 <= gpu->fill_y1))
        {
            gpu->fill_key = fill_key;
            return;
        }

        for (int32_t y = yfirst; y < y1; y += ystep)
        {
            uint16_t *dst = &gpu->vram[PSX_VRAM_AT(x0, y)];
            gpu_fill_span(dst, solid_color, rect_w);
        }

        gpu->fill_key = key;
        gpu->fill_x0 = x0;
        gpu->fill_y0 = yfirst;
        gpu->fill_x1 = x1;
        gpu->fill_y1 = y1;
        return;
    }

    if (!is_textured)
    {
        for (int32_t y = yfirst; y < y1; y += ystep)
        {
            uint16_t *dst = &gpu->vram[PSX_VRAM_AT(x0, y)];
            for (int32_t i = 0; i < rect_w; ++i, ++dst)
            {
                *dst = gpu_blend_bgr555(solid_color, *dst, transp_mode);
            }
        }
        return;
    }

    const int mod_r = (data.v0.c >> 0) & 0xff;
    const int mod_g = (data.v0.c >> 8) & 0xff;
    const int mod_b = (data.v0.c >> 16) & 0xff;
    const int32_t tex_start_x = data.v0.tx + (x0 - screen_x0);
    int32_t tex_y = data.v0.ty + (y0 - screen_y0);
    const uint16_t *const clut = gpu_clut_ptr(gpu, depth, clutx, cluty,
                                              (uint32_t)rect_w * (uint32_t)(y1 - y0));

    /*
        The rectangle a game actually draws: a sprite or a background tile in the
        texture's own colours (raw, or modulated by the neutral 0x808080, which
        leaves every 5 bit channel as it was), no texture window, no wrap around
        the 256 texel page. FF7's field screens are a few hundred of these per
        frame and they were the single biggest item in the rasterizer: the
        general loop below runs the texture window arithmetic, a switch on the
        texture depth and three multiplies with saturation for every pixel, only
        to arrive at the texel it started with.

        Texels and the palette are read per pixel, in the same order as below,
        so even a rectangle that overlaps its own texture comes out the same.
        Checked against the general loop on the host, bit for bit.
    */
    const int neutral = is_raw || ((mod_r == 0x80) && (mod_g == 0x80) && (mod_b == 0x80));

    if (neutral && !gpu->texw_mx && !gpu->texw_my && (tex_start_x >= 0) &&
        ((tex_start_x + rect_w) <= 256) && (tex_y >= 0) && ((tex_y + (y1 - y0)) <= 256) &&
        /* and not past the right edge of VRAM, where the texture rows wrap */
        ((tpx + ((tex_start_x + rect_w - 1) >> ((depth == 0) ? 2 : ((depth == 1) ? 1 : 0)))) < 1024))
    {
        /* modulation drops the texel's mask bit (rgb888_to_bgr555 has none) */
        const uint16_t keep = is_raw ? 0xffffu : 0x7fffu;

#if PSX_PROFILE
        g_prof.px_fast += (uint32_t)rect_w * (uint32_t)(y1 - y0);
#endif

        /* which halfword of a texture row a texel sits in */
        const int tshift = (depth == 0) ? 2 : ((depth == 1) ? 1 : 0);

        /* whether the rectangle covers part of its own texture (4 bit texels are read
           two at a time below, which is only the same when it does not) */
        const int self = (depth == 0) && ((tpy + tex_y) < y1) && (y0 < (tpy + tex_y + (y1 - y0))) &&
                         ((tpx + (tex_start_x >> 2)) < (x0 + rect_w)) &&
                         (x0 <= (tpx + ((tex_start_x + rect_w - 1) >> 2)));

        tex_y += yfirst - y0;

        for (int32_t y = yfirst; y < y1; y += ystep, tex_y += ystep)
        {
            uint16_t *dst = &gpu->vram[PSX_VRAM_AT(x0, y)];
            const uint16_t *const trow = &gpu->vram[PSX_VRAM_AT(tpx, tpy + tex_y)];

            /* Textures live in SDRAM, and a row that is not in the cache costs
               a line fill of some hundred cycles - about as much as drawing a
               16 pixel row. The core fetches a preloaded line in the background,
               so asking for the next row now hides that wait behind this row's
               work. (Not for the frame buffer: whole line writes do not wait
               for a fill, and a preload only gets in their way - measured.) */
            if ((y + ystep) < y1)
            {
                __builtin_prefetch(trow + (PSX_GPU_VRAM_PITCH * ystep) + (tex_start_x >> tshift));
                __builtin_prefetch(trow + (PSX_GPU_VRAM_PITCH * ystep) + ((tex_start_x + rect_w - 1) >> tshift));
            }

            /* A texel is read where it lies in the row: an 8 bit one is the byte at its
               x, a 4 bit one a nibble of the byte at x / 2 (low nibble first) - the
               same texels as the halfword arithmetic, without it */
#define GPU_SPRITE_PUT(i, texel)                                                   \
            if (texel)                                                             \
            {                                                                      \
                const uint16_t out_ = (uint16_t)((texel) & keep);                  \
                                                                                   \
                if (__builtin_expect(base_transp && ((texel) & 0x8000u), 0))       \
                    dst[i] = gpu_blend_bgr555(out_, dst[i], transp_mode);          \
                else                                                               \
                    dst[i] = out_;                                                 \
            }

            if (depth == 1)
            {
                const uint8_t *const t8 = (const uint8_t *)trow + tex_start_x;

                for (int32_t i = 0; i < rect_w; i++)
                {
                    const uint32_t texel = clut[t8[i]];

                    GPU_SPRITE_PUT(i, texel)
                }
            }
            else if ((depth == 0) && self)
            {
                /* drawing over its own texture: every texel read after the pixel before it is written */
                for (int32_t i = 0, tx = tex_start_x; i < rect_w; i++, tx++)
                {
                    const uint32_t texel = clut[(trow[tx >> 2] >> ((tx & 3) << 2)) & 0xfu];

                    GPU_SPRITE_PUT(i, texel)
                }
            }
            else if (depth == 0)
            {
                const uint8_t *t4 = (const uint8_t *)trow + (tex_start_x >> 1);
                int32_t i = 0;

                if (tex_start_x & 1)
                {
                    const uint32_t texel = clut[*t4++ >> 4];

                    GPU_SPRITE_PUT(0, texel)
                    i = 1;
                }

                for (; (i + 1) < rect_w; i += 2)
                {
                    const uint32_t two = *t4++;
                    const uint32_t texel0 = clut[two & 0xfu];
                    const uint32_t texel1 = clut[two >> 4];

                    GPU_SPRITE_PUT(i, texel0)
                    GPU_SPRITE_PUT(i + 1, texel1)
                }

                if (i < rect_w)
                {
                    const uint32_t texel = clut[*t4 & 0xfu];

                    GPU_SPRITE_PUT(i, texel)
                }
            }
            else
            {
                const uint16_t *const t16 = trow + tex_start_x;

                for (int32_t i = 0; i < rect_w; i++)
                {
                    const uint32_t texel = t16[i];

                    GPU_SPRITE_PUT(i, texel)
                }
            }

#undef GPU_SPRITE_PUT
        }

        return;
    }

    tex_y += yfirst - y0;

    for (int32_t y = yfirst; y < y1; y += ystep, tex_y += ystep)
    {
        uint16_t *dst = &gpu->vram[PSX_VRAM_AT(x0, y)];
        int32_t tex_x = tex_start_x;

        for (int32_t i = 0; i < rect_w; ++i, ++tex_x, ++dst)
        {
            const uint16_t texel = gpu_fetch_texel(
                gpu,
                tex_x, tex_y,
                tpx, tpy, clut, depth);

            if (!texel)
                continue;

            uint16_t out_color;
            if (is_raw)
            {
                out_color = texel;
            }
            else
            {
                const int tr = ((texel >> 0) & 0x1f) << 3;
                const int tg = ((texel >> 5) & 0x1f) << 3;
                const int tb = ((texel >> 10) & 0x1f) << 3;
                int cr = (tr * mod_r) >> 7;
                int cg = (tg * mod_g) >> 7;
                int cb = (tb * mod_b) >> 7;
                unsigned int ucr = fast_saturate_u8(cr);
                unsigned int ucg = fast_saturate_u8(cg);
                unsigned int ucb = fast_saturate_u8(cb);
                uint32_t rgb = ucr | (ucg << 8) | (ucb << 16);
                out_color = rgb888_to_bgr555(rgb);
            }

            if (__builtin_expect(base_transp && (texel & 0x8000), 0))
            {
                *dst = gpu_blend_bgr555(out_color, *dst, transp_mode);
            }
            else
            {
                *dst = out_color;
            }
        }
    }
}

PSX_GPU_HOT void plotLineLow(psx_gpu_t *gpu, int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int yi = 1;
    if (dy < 0)
    {
        yi = -1;
        dy = -dy;
    }
    int d = (2 * dy) - dx;
    int y = y0;

    for (int x = x0; x < x1; x++)
    {
        int bc = (x >= gpu->draw_x1) && (x <= gpu->draw_x2) &&
                 (y >= gpu->draw_ry1) && (y <= gpu->draw_ry2);

        if ((x < 1024) && (y < 512) && (x >= 0) && (y >= 0) && bc && ((y & 1) != gpu->skip_rows))
            gpu->vram[PSX_VRAM_AT(x, y)] = color;

        if (d > 0)
        {
            y += yi;
            d += (2 * (dy - dx));
        }
        else
        {
            d += 2 * dy;
        }
    }
}

PSX_GPU_HOT void plotLineHigh(psx_gpu_t *gpu, int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int xi = 1;
    if (dx < 0)
    {
        xi = -1;
        dx = -dx;
    }
    int d = (2 * dx) - dy;
    int x = x0;

    for (int y = y0; y < y1; y++)
    {
        int bc = (x >= gpu->draw_x1) && (x <= gpu->draw_x2) &&
                 (y >= gpu->draw_ry1) && (y <= gpu->draw_ry2);

        if ((x < 1024) && (y < 512) && (x >= 0) && (y >= 0) && bc && ((y & 1) != gpu->skip_rows))
            gpu->vram[PSX_VRAM_AT(x, y)] = color;

        if (d > 0)
        {
            x = x + xi;
            d += (2 * (dx - dy));
        }
        else
        {
            d += 2 * dx;
        }
    }
}

PSX_GPU_HOT void plotLine(psx_gpu_t *gpu, int x0, int y0, int x1, int y1, uint16_t color)
{
    if (abs(y1 - y0) < abs(x1 - x0))
    {
        if (x0 > x1)
        {
            plotLineLow(gpu, x1, y1, x0, y0, color);
        }
        else
        {
            plotLineLow(gpu, x0, y0, x1, y1, color);
        }
    }
    else
    {
        if (y0 > y1)
        {
            plotLineHigh(gpu, x1, y1, x0, y0, color);
        }
        else
        {
            plotLineHigh(gpu, x0, y0, x1, y1, color);
        }
    }
}

PSX_GPU_HOT void gpu_render_flat_line(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, uint32_t color)
{
    /* the field nobody sees (gpu_update_hidden); lines are clipped to the drawing area too */
    if (gpu->skip_prims)
        return;

#if PSX_GPU_EXTERNAL_RASTER
    if (psx_raster_line(gpu, v0, v1, (uint16_t)color, 0))
    {
        gpu_touch(gpu, 0, (uint32_t)gpu->draw_y1, PSX_GPU_FB_WIDTH, (uint32_t)gpu->draw_y2 + 1u);

        return;
    }
#endif

    v0.x += gpu->off_x;
    v0.y += gpu->off_y;
    v1.x += gpu->off_x;
    v1.y += gpu->off_y;

    plotLine(gpu, v0.x, v0.y, v1.x, v1.y, color);
}

void gpu_render_flat_rectangle(psx_gpu_t *gpu, vertex_t v, uint32_t w, uint32_t h, uint32_t color)
{
    /* Offset coordinates */
    v.x += gpu->off_x;
    v.y += gpu->off_y;

    /* Calculate bounding box */
    int xmin = max(v.x, gpu->draw_x1);
    int ymin = max(v.y, gpu->draw_ry1);
    int xmax = min(xmin + w, gpu->draw_x2);
    int ymax = min(ymin + h, gpu->draw_ry2);

    /* Early exit if clipped completely */
    if (xmin >= xmax || ymin >= ymax)
        return;

    uint32_t rect_width = xmax - xmin;
    uint16_t color16 = (uint16_t)color;

    /* Fast rectangle filling inspired by pushBlock16 approach */
    for (uint32_t y = ymin; y < ymax; y++)
    {
        uint16_t *line_ptr = &gpu->vram[PSX_VRAM_AT(xmin, y)];
        uint32_t len = rect_width;

        /* Unrolled loop for better performance - process 32 pixels at once */
        while (len > 31)
        {
            line_ptr[0] = color16;
            line_ptr[1] = color16;
            line_ptr[2] = color16;
            line_ptr[3] = color16;
            line_ptr[4] = color16;
            line_ptr[5] = color16;
            line_ptr[6] = color16;
            line_ptr[7] = color16;
            line_ptr[8] = color16;
            line_ptr[9] = color16;
            line_ptr[10] = color16;
            line_ptr[11] = color16;
            line_ptr[12] = color16;
            line_ptr[13] = color16;
            line_ptr[14] = color16;
            line_ptr[15] = color16;
            line_ptr[16] = color16;
            line_ptr[17] = color16;
            line_ptr[18] = color16;
            line_ptr[19] = color16;
            line_ptr[20] = color16;
            line_ptr[21] = color16;
            line_ptr[22] = color16;
            line_ptr[23] = color16;
            line_ptr[24] = color16;
            line_ptr[25] = color16;
            line_ptr[26] = color16;
            line_ptr[27] = color16;
            line_ptr[28] = color16;
            line_ptr[29] = color16;
            line_ptr[30] = color16;
            line_ptr[31] = color16;
            line_ptr += 32;
            len -= 32;
        }

        /* Process 8 pixels at once */
        while (len > 7)
        {
            line_ptr[0] = color16;
            line_ptr[1] = color16;
            line_ptr[2] = color16;
            line_ptr[3] = color16;
            line_ptr[4] = color16;
            line_ptr[5] = color16;
            line_ptr[6] = color16;
            line_ptr[7] = color16;
            line_ptr += 8;
            len -= 8;
        }

        /* Process remaining pixels */
        while (len--)
        {
            *line_ptr++ = color16;
        }
    }
}

void gpu_render_textured_rectangle(psx_gpu_t *gpu, vertex_t v, uint32_t w, uint32_t h, uint16_t clutx, uint16_t cluty, uint32_t color)
{
    vertex_t a = v;

    a.x += gpu->off_x;
    a.y += gpu->off_y;

    int xmin = max(a.x, gpu->draw_x1);
    int ymin = max(a.y, gpu->draw_ry1);
    int xmax = min(xmin + w, gpu->draw_x2);
    int ymax = min(ymin + h, gpu->draw_ry2);

    uint32_t xc = 0, yc = 0;

    const uint16_t *const clut = gpu_clut_ptr(gpu, gpu->texp_d, clutx, cluty,
                                              (uint32_t)((xmax - xmin) * (ymax - ymin)));

    for (int y = ymin; y < ymax; y++)
    {
        for (int x = xmin; x < xmax; x++)
        {
            uint16_t texel = gpu_fetch_texel(
                gpu,
                a.tx + xc, a.ty + yc,
                gpu->texp_x, gpu->texp_y,
                clut,
                gpu->texp_d);

            ++xc;

            gpu->vram[PSX_VRAM_AT(x, y)] = texel;
        }

        xc = 0;

        ++yc;
    }
}

void gpu_render_flat_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, uint32_t color)
{
    vertex_t a, b, c;
    uint16_t rgb = color & 0xFFFF;

    a = v0;

    /* Ensure the winding order is correct */
    if (EDGE(v0, v1, v2) < 0)
    {
        b = v2;
        c = v1;
    }
    else
    {
        b = v1;
        c = v2;
    }

    a.x += gpu->off_x;
    a.y += gpu->off_y;
    b.x += gpu->off_x;
    b.y += gpu->off_y;
    c.x += gpu->off_x;
    c.y += gpu->off_y;

    int xmin = max(min(min(a.x, b.x), c.x), gpu->draw_x1);
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_ry1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2);
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_ry2);

    for (int y = ymin; y < ymax; y++)
    {
        for (int x = xmin; x < xmax; x++)
        {
            int z0 = ((b.x - a.x) * (y - a.y)) - ((b.y - a.y) * (x - a.x));
            int z1 = ((c.x - b.x) * (y - b.y)) - ((c.y - b.y) * (x - b.x));
            int z2 = ((a.x - c.x) * (y - c.y)) - ((a.y - c.y) * (x - c.x));

            if ((z0 >= 0) && (z1 >= 0) && (z2 >= 0))
            {
                gpu->vram[PSX_VRAM_AT(x, y)] = rgb;
            }
        }
    }
}

void gpu_render_shaded_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2)
{
    vertex_t a, b, c, p;

    a = v0;

    /* Ensure the winding order is correct */
    if (EDGE(v0, v1, v2) < 0)
    {
        b = v2;
        c = v1;
    }
    else
    {
        b = v1;
        c = v2;
    }

    a.x += gpu->off_x;
    a.y += gpu->off_y;
    b.x += gpu->off_x;
    b.y += gpu->off_y;
    c.x += gpu->off_x;
    c.y += gpu->off_y;

    int xmin = max(min(min(a.x, b.x), c.x), gpu->draw_x1);
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_ry1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2);
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_ry2);

    int area = EDGE(a, b, c);

    for (int y = ymin; y < ymax; y++)
    {
        for (int x = xmin; x < xmax; x++)
        {
            p.x = x;
            p.y = y;

            float z0 = EDGE((float)b, (float)c, (float)p);
            float z1 = EDGE((float)c, (float)a, (float)p);
            float z2 = EDGE((float)a, (float)b, (float)p);

            if ((z0 >= 0) && (z1 >= 0) && (z2 >= 0))
            {
                int cr = (z0 * ((a.c >> 0) & 0xff) + z1 * ((b.c >> 0) & 0xff) + z2 * ((c.c >> 0) & 0xff)) / area;
                int cg = (z0 * ((a.c >> 8) & 0xff) + z1 * ((b.c >> 8) & 0xff) + z2 * ((c.c >> 8) & 0xff)) / area;
                int cb = (z0 * ((a.c >> 16) & 0xff) + z1 * ((b.c >> 16) & 0xff) + z2 * ((c.c >> 16) & 0xff)) / area;

                // Calculate positions within our 4x4 dither
                // kernel
                int dy = (y - ymin) % 4;
                int dx = (x - xmin) % 4;

                // Shift two pixels horizontally on the last
                // two scanlines?
                // if (dy > 1) {
                //     dx = ((x + 2) - xmin) % 4;
                // }

                int dither = g_psx_gpu_dither_kernel[dx + (dy * 4)];

                // Add to the original 8-bit color values
                cr += dither;
                cg += dither;
                cb += dither;

                // Saturate (clamp) to 00-ff
                cr = (cr >= 0xff) ? 0xff : ((cr <= 0) ? 0 : cr);
                cg = (cg >= 0xff) ? 0xff : ((cg <= 0) ? 0 : cg);
                cb = (cb >= 0xff) ? 0xff : ((cb <= 0) ? 0 : cb);

                uint32_t color = (cb << 16) | (cg << 8) | cr;

                gpu->vram[PSX_VRAM_AT(x, y)] = rgb888_to_bgr555(color);
            }
        }
    }
}

void gpu_render_textured_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, uint32_t tpx, uint32_t tpy, uint16_t clutx, uint16_t cluty, int depth)
{
    vertex_t a, b, c;

    a = v0;

    /* Ensure the winding order is correct */
    if (EDGE(v0, v1, v2) < 0)
    {
        b = v2;
        c = v1;
    }
    else
    {
        b = v1;
        c = v2;
    }

    a.x += gpu->off_x;
    a.y += gpu->off_y;
    b.x += gpu->off_x;
    b.y += gpu->off_y;
    c.x += gpu->off_x;
    c.y += gpu->off_y;

    int xmin = max(min(min(a.x, b.x), c.x), gpu->draw_x1);
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_ry1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2);
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_ry2);

    const uint16_t *const clut = gpu_clut_ptr(gpu, depth, clutx, cluty,
                                              (uint32_t)((xmax - xmin + 1) * (ymax - ymin + 1)));

    uint32_t area = EDGE(a, b, c);

    for (int y = ymin; y < ymax; y++)
    {
        for (int x = xmin; x < xmax; x++)
        {
            vertex_t p;

            p.x = x;
            p.y = y;

            float z0 = EDGE((float)b, (float)c, (float)p);
            float z1 = EDGE((float)c, (float)a, (float)p);
            float z2 = EDGE((float)a, (float)b, (float)p);

            if ((z0 >= 0) && (z1 >= 0) && (z2 >= 0))
            {
                uint32_t tx = ((z0 * a.tx) + (z1 * b.tx) + (z2 * c.tx)) / area;
                uint32_t ty = ((z0 * a.ty) + (z1 * b.ty) + (z2 * c.ty)) / area;

                uint16_t color = gpu_fetch_texel(
                    gpu,
                    tx, ty,
                    tpx, tpy,
                    clut,
                    depth);

                if (!color)
                    continue;

                gpu->vram[PSX_VRAM_AT(x, y)] = rgb888_to_bgr555(color);
            }
        }
    }
}

#define I32(v, b) (((int32_t)((v) << (31 - b))) >> (31 - b))

#if PSX_PROFILE
#define gpu_render_rect(g, r)                                   \
    do                                                          \
    {                                                           \
        const uint32_t t0_ = DWT->CYCCNT;                       \
        const uint32_t px_ = g_prof.px_rect;                    \
        gpu_render_rect((g), (r));                              \
        g_prof.ras_cyc[7] += DWT->CYCCNT - t0_;                 \
        g_prof.ras_cnt[7]++;                                    \
        g_prof.ras_px[7] += g_prof.px_rect - px_;               \
    } while (0)
#endif

PSX_GPU_HOT void gpu_rect(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;

        int size = (gpu->buf[0] >> 27) & 3;
        int textured = (gpu->buf[0] & 0x04000000) != 0;

        gpu->cmd_args_remaining = 1 + (size == RS_VARIABLE) + textured;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            rect_data_t rect;

            rect.attrib = gpu->buf[0] >> 24;

            int textured = (rect.attrib & RA_TEXTURED) != 0;
            int raw = (rect.attrib & RA_RAW) != 0;
            int size_code = (rect.attrib >> 3) & 3;

            // Add 1 if is textured
            int size_offset = 2 + textured;

            rect.v0.c = gpu->buf[0] & 0xffffff;
            rect.v0.x = SE10(gpu->buf[1] & 0xffff);
            rect.v0.y = SE10(gpu->buf[1] >> 16);
            rect.v0.tx = (gpu->buf[2] >> 0) & 0xff;
            rect.v0.ty = (gpu->buf[2] >> 8) & 0xff;
            rect.clut = gpu->buf[2] >> 16;

            static const uint16_t rect_size_lut[4] = {0, 1, 8, 16};

            if (size_code == RS_VARIABLE)
            {
                rect.width = gpu->buf[size_offset] & 0xffff;
                rect.height = gpu->buf[size_offset] >> 16;
            }
            else
            {
                rect.width = rect_size_lut[size_code];
                rect.height = rect_size_lut[size_code];
            }

            if (textured && raw)
                rect.v0.c = 0x808080;

            gpu_render_rect(gpu, rect);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_poly(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;

        int shaded = (gpu->buf[0] & 0x10000000) != 0;
        int quad = (gpu->buf[0] & 0x08000000) != 0;
        int textured = (gpu->buf[0] & 0x04000000) != 0;

        int fields_per_vertex = 1 + shaded + textured;
        int vertices = 3 + quad;

        gpu->cmd_args_remaining = (fields_per_vertex * vertices) - shaded;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            poly_data_t poly;

            poly.attrib = gpu->buf[0] >> 24;

            int shaded = (poly.attrib & PA_SHADED) != 0;
            int textured = (poly.attrib & PA_TEXTURED) != 0;

            int color_offset = shaded * (2 + textured);
            int vert_offset = 1 + (textured | shaded) +
                              (textured & shaded);
            int texc_offset = textured * (2 + shaded);
            int texp_offset = textured * (4 + shaded);

            poly.clut = gpu->buf[2] >> 16;
            poly.texp = gpu->buf[texp_offset] >> 16;

            // Undocumented behavior?
            // Fixes Mortal Kombat II, Bubble Bobble, Driver 1 & 2
            if (textured)
            {
#if PSX_PROFILE
                {
                    static uint32_t last_texp = 0xffffffffu;

                    if ((poly.texp & 0x1ffu) != last_texp)
                    {
                        last_texp = poly.texp & 0x1ffu;
                        g_prof.tex_switch++;
                    }
                }
#endif
                gpu->texp_x = (poly.texp & 0xf) << 6;
                gpu->texp_y = (poly.texp & 0x10) << 4;
                gpu->texp_d = (poly.texp >> 7) & 0x3;
                gpu->gpustat &= 0xfffffe00;
                gpu->gpustat |= poly.texp & 0x1ff;
            }

            poly.v[0].c = gpu->buf[0 + 0 * color_offset] & 0xffffff;
            poly.v[1].c = gpu->buf[0 + 1 * color_offset] & 0xffffff;
            poly.v[2].c = gpu->buf[0 + 2 * color_offset] & 0xffffff;
            poly.v[3].c = gpu->buf[0 + 3 * color_offset] & 0xffffff;
            poly.v[0].x = SE10(gpu->buf[1 + 0 * vert_offset] & 0xffff);
            poly.v[1].x = SE10(gpu->buf[1 + 1 * vert_offset] & 0xffff);
            poly.v[2].x = SE10(gpu->buf[1 + 2 * vert_offset] & 0xffff);
            poly.v[3].x = SE10(gpu->buf[1 + 3 * vert_offset] & 0xffff);
            poly.v[0].y = SE10(gpu->buf[1 + 0 * vert_offset] >> 16);
            poly.v[1].y = SE10(gpu->buf[1 + 1 * vert_offset] >> 16);
            poly.v[2].y = SE10(gpu->buf[1 + 2 * vert_offset] >> 16);
            poly.v[3].y = SE10(gpu->buf[1 + 3 * vert_offset] >> 16);
            poly.v[0].tx = gpu->buf[2 + 0 * texc_offset] & 0xff;
            poly.v[1].tx = gpu->buf[2 + 1 * texc_offset] & 0xff;
            poly.v[2].tx = gpu->buf[2 + 2 * texc_offset] & 0xff;
            poly.v[3].tx = gpu->buf[2 + 3 * texc_offset] & 0xff;
            poly.v[0].ty = (gpu->buf[2 + 0 * texc_offset] >> 8) & 0xff;
            poly.v[1].ty = (gpu->buf[2 + 1 * texc_offset] >> 8) & 0xff;
            poly.v[2].ty = (gpu->buf[2 + 2 * texc_offset] >> 8) & 0xff;
            poly.v[3].ty = (gpu->buf[2 + 3 * texc_offset] >> 8) & 0xff;

            if (poly.attrib & PA_QUAD)
            {
                gpu_render_triangle(gpu, poly.v[0], poly.v[1], poly.v[2], poly, 1);
                gpu_render_triangle(gpu, poly.v[1], poly.v[2], poly.v[3], poly, 1);
            }
            else
            {
                gpu_render_triangle(gpu, poly.v[0], poly.v[1], poly.v[2], poly, 0);
            }

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_line(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;

        int shaded = (gpu->buf[0] & 0x10000000) != 0;
        int polyline = (gpu->buf[0] & 0x08000000) != 0;

        gpu->cmd_args_remaining = polyline ? -1 : (shaded ? 3 : 2);
        gpu->line_done = 0;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (gpu->buf[0] & 0x08000000)
        {
            if ((gpu->buf[gpu->buf_index - 1] & 0xf000f000) == 0x50005000)
            {
                gpu->state = GPU_STATE_RECV_CMD;

                return;
            }
        }
        else if (!gpu->cmd_args_remaining)
        {
            vertex_t v0, v1;

            if (gpu->buf[0] & 0x10000000)
            {
                v0.c = gpu->buf[0] & 0xffffff;
                v1.c = gpu->buf[2] & 0xffffff;
                v0.x = gpu->buf[1] & 0xffff;
                v0.y = gpu->buf[1] >> 16;
                v1.x = gpu->buf[3] & 0xffff;
                v1.y = gpu->buf[3] >> 16;
            }
            else
            {
                v0.c = gpu->buf[0] & 0xffffff;
                v1.c = gpu->buf[0] & 0xffffff;
                v0.x = gpu->buf[1] & 0xffff;
                v0.y = gpu->buf[1] >> 16;
                v1.x = gpu->buf[2] & 0xffff;
                v1.y = gpu->buf[2] >> 16;
            }

            gpu_render_flat_line(gpu, v0, v1, rgb888_to_bgr555(gpu->buf[0] & 0xffffff));

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_cmd_a0(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 2;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            // Save static data
            gpu->xpos = gpu->buf[1] & 0x3ff;
            gpu->ypos = (gpu->buf[1] >> 16) & 0x1ff;
            gpu->xsiz = gpu->buf[2] & 0xffff;
            gpu->ysiz = gpu->buf[2] >> 16;
            gpu->xsiz = ((gpu->xsiz - 1) & 0x3ff) + 1;
            gpu->ysiz = ((gpu->ysiz - 1) & 0x1ff) + 1;
            gpu->tsiz = ((gpu->xsiz * gpu->ysiz) + 1) & 0xfffffffe;
            gpu->addr = gpu->xpos + (gpu->ypos * 1024);

#if PSX_GPU_EXTERNAL_RASTER
            gpu_img_rects(gpu);
#endif

            PSX_RASTER_SYNC();
            gpu->xcnt = 0;
            gpu->ycnt = 0;
        }
    }
    break;

    case GPU_STATE_RECV_DATA:
    {
        /* Judged as a whole rectangle, but with every word: a transfer can
           straddle a presented frame, and what arrives after that has to
           raise the flag again. One test while the flag is already up. */
        gpu_touch(gpu, gpu->xpos, gpu->ypos, gpu->xpos + gpu->xsiz, gpu->ypos + gpu->ysiz);
        gpu_reader_guard_rows(gpu->ypos, gpu->ysiz);

        unsigned int xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
        unsigned int ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;

        gpu->vram[PSX_VRAM_AT(xpos, ypos)] = PSX_UPLOAD_PIX(gpu, xpos, ypos, gpu->recv_data & 0xffff);

        ++gpu->xcnt;

        xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
        ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;

        if (gpu->xcnt == gpu->xsiz)
        {
            ++gpu->ycnt;
            gpu->xcnt = 0;

            ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;
            xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
        }

        gpu->vram[PSX_VRAM_AT(xpos, ypos)] = PSX_UPLOAD_PIX(gpu, xpos, ypos, gpu->recv_data >> 16);

        ++gpu->xcnt;

        if (gpu->xcnt == gpu->xsiz)
        {
            ++gpu->ycnt;
            gpu->xcnt = 0;

            xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
            ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;
        }

        gpu->tsiz -= 2;

        if (!gpu->tsiz)
        {
            gpu->xcnt = 0;
            gpu->ycnt = 0;
            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

/*
    Bulk CPU -> VRAM upload.

    A DMA driven upload (command A0h) hands the GPU one word per call, and each
    of those runs the whole command state machine to write two pixels. Video is
    exactly that, word after word, and it dominated the DMA time: FF7's full
    motion video pushes over a megabyte a second this way.

    The data is already in the native VRAM pixel format, so whole rows can be
    copied instead. This takes the common shape of such a transfer - starting at
    a row boundary, an even number of pixels per row, an even destination, no
    wrap in x - and copies row by row, leaving anything else to the per word
    path, which stays the reference for correctness.

    Returns the number of 32 bit words consumed, which may be zero.
*/
uint32_t PSX_GPU_HOT psx_gpu_write_bulk(psx_gpu_t *gpu, const uint32_t *src, uint32_t words)
{
    PSX_RASTER_SYNC();

#if PSXE_GPU_REMOTE
    if (g_gpu_remote && g_gpu_stream)
        return gpu_remote_gp0_bulk(gpu, src, words);
#endif

    if (gpu->state != GPU_STATE_RECV_DATA)
        return 0;

    gpu_reader_guard_rows(gpu->ypos, gpu->ysiz);

    uint32_t used = 0;

    while (words)
    {
        const uint32_t row = gpu->xsiz;

        if (gpu->xcnt || (row & 1u) || (gpu->xpos & 1u))
            break;

        const uint32_t row_words = row >> 1;

        if (!row_words || (words < row_words) || (gpu->tsiz < row))
            break;

        if ((gpu->xpos + row) > 1024u)
            break;

        const uint32_t ypos = (gpu->ypos + gpu->ycnt) & 0x1ffu;

#if PSX_GPU_EXTERNAL_RASTER
        gpu_upload_row(gpu, &gpu->vram[PSX_VRAM_AT(gpu->xpos, ypos)], (const uint16_t *)src, gpu->xpos, ypos, row);
#else
        memcpy(&gpu->vram[PSX_VRAM_AT(gpu->xpos, ypos)], src, row_words * 4u);
#endif

        src += row_words;
        used += row_words;
        words -= row_words;

        gpu->tsiz -= row;
        gpu->ycnt++;

        if (!gpu->tsiz)
        {
            gpu->xcnt = 0;
            gpu->ycnt = 0;
            gpu->state = GPU_STATE_RECV_CMD;

            break;
        }
    }

    if (used)
        gpu_touch(gpu, gpu->xpos, gpu->ypos, gpu->xpos + gpu->xsiz, gpu->ypos + gpu->ysiz);

#if PSXE_GPU_REMOTE
    if (g_gpu_stream)
        gpu_remote_note_bulk(used);
#endif

    return used;
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_28(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 4;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v1.x = gpu->buf[2] & 0xffff;
            gpu->v1.y = gpu->buf[2] >> 16;
            gpu->v2.x = gpu->buf[3] & 0xffff;
            gpu->v2.y = gpu->buf[3] >> 16;
            gpu->v3.x = gpu->buf[4] & 0xffff;
            gpu->v3.y = gpu->buf[4] >> 16;
            gpu->color = gpu->buf[0] & 0xffffff;

            gpu_render_flat_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, rgb888_to_bgr555(gpu->color));
            gpu_render_flat_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, rgb888_to_bgr555(gpu->color));

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_30(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 5;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->v0.c = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v1.c = gpu->buf[2] & 0xffffff;
            gpu->v1.x = gpu->buf[3] & 0xffff;
            gpu->v1.y = gpu->buf[3] >> 16;
            gpu->v2.c = gpu->buf[4] & 0xffffff;
            gpu->v2.x = gpu->buf[5] & 0xffff;
            gpu->v2.y = gpu->buf[5] >> 16;

            gpu_render_shaded_triangle(gpu, gpu->v0, gpu->v1, gpu->v2);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_38(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 7;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->v0.c = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v1.c = gpu->buf[2] & 0xffffff;
            gpu->v1.x = gpu->buf[3] & 0xffff;
            gpu->v1.y = gpu->buf[3] >> 16;
            gpu->v2.c = gpu->buf[4] & 0xffffff;
            gpu->v2.x = gpu->buf[5] & 0xffff;
            gpu->v2.y = gpu->buf[5] >> 16;
            gpu->v3.c = gpu->buf[6] & 0xffffff;
            gpu->v3.x = gpu->buf[7] & 0xffff;
            gpu->v3.y = gpu->buf[7] >> 16;

            gpu_render_shaded_triangle(gpu, gpu->v0, gpu->v1, gpu->v2);
            gpu_render_shaded_triangle(gpu, gpu->v1, gpu->v2, gpu->v3);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_3c(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 11;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            uint32_t texp = gpu->buf[5] >> 16;
            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->pal = gpu->buf[2] >> 16;
            gpu->v0.tx = gpu->buf[2] & 0xff;
            gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
            gpu->v1.tx = gpu->buf[5] & 0xff;
            gpu->v1.ty = (gpu->buf[5] >> 8) & 0xff;
            gpu->v2.tx = gpu->buf[8] & 0xff;
            gpu->v2.ty = (gpu->buf[8] >> 8) & 0xff;
            gpu->v3.tx = gpu->buf[11] & 0xff;
            gpu->v3.ty = (gpu->buf[11] >> 8) & 0xff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v1.x = gpu->buf[4] & 0xffff;
            gpu->v1.y = gpu->buf[4] >> 16;
            gpu->v2.x = gpu->buf[7] & 0xffff;
            gpu->v2.y = gpu->buf[7] >> 16;
            gpu->v3.x = gpu->buf[10] & 0xffff;
            gpu->v3.y = gpu->buf[10] >> 16;

            uint16_t clutx = (gpu->pal & 0x3f) << 4;
            uint16_t cluty = (gpu->pal >> 6) & 0x1ff;
            uint16_t tpx = (texp & 0xf) << 6;
            uint16_t tpy = (texp & 0x10) << 4;
            uint16_t depth = (texp >> 7) & 0x3;

            gpu_render_textured_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, tpx, tpy, clutx, cluty, depth);
            gpu_render_textured_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, tpx, tpy, clutx, cluty, depth);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_2c(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 8;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            uint32_t texp = gpu->buf[4] >> 16;
            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->pal = gpu->buf[2] >> 16;
            gpu->v0.tx = gpu->buf[2] & 0xff;
            gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
            gpu->v1.tx = gpu->buf[4] & 0xff;
            gpu->v1.ty = (gpu->buf[4] >> 8) & 0xff;
            gpu->v2.tx = gpu->buf[6] & 0xff;
            gpu->v2.ty = (gpu->buf[6] >> 8) & 0xff;
            gpu->v3.tx = gpu->buf[8] & 0xff;
            gpu->v3.ty = (gpu->buf[8] >> 8) & 0xff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v1.x = gpu->buf[3] & 0xffff;
            gpu->v1.y = gpu->buf[3] >> 16;
            gpu->v2.x = gpu->buf[5] & 0xffff;
            gpu->v2.y = gpu->buf[5] >> 16;
            gpu->v3.x = gpu->buf[7] & 0xffff;
            gpu->v3.y = gpu->buf[7] >> 16;

            uint16_t clutx = (gpu->pal & 0x3f) << 4;
            uint16_t cluty = (gpu->pal >> 6) & 0x1ff;
            uint16_t tpx = (texp & 0xf) << 6;
            uint16_t tpy = (texp & 0x10) << 4;
            uint16_t depth = (texp >> 7) & 0x3;

            gpu_render_textured_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, tpx, tpy, clutx, cluty, depth);
            gpu_render_textured_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, tpx, tpy, clutx, cluty, depth);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_24(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 6;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            uint32_t texp = gpu->buf[4] >> 16;
            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->pal = gpu->buf[2] >> 16;
            gpu->v0.tx = gpu->buf[2] & 0xff;
            gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
            gpu->v1.tx = gpu->buf[4] & 0xff;
            gpu->v1.ty = (gpu->buf[4] >> 8) & 0xff;
            gpu->v2.tx = gpu->buf[6] & 0xff;
            gpu->v2.ty = (gpu->buf[6] >> 8) & 0xff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v1.x = gpu->buf[3] & 0xffff;
            gpu->v1.y = gpu->buf[3] >> 16;
            gpu->v2.x = gpu->buf[5] & 0xffff;
            gpu->v2.y = gpu->buf[5] >> 16;

            uint16_t clutx = (gpu->pal & 0x3f) << 4;
            uint16_t cluty = (gpu->pal >> 6) & 0x1ff;
            uint16_t tpx = (texp & 0xf) << 6;
            uint16_t tpy = (texp & 0x10) << 4;
            uint16_t depth = (texp >> 7) & 0x3;

            gpu_render_textured_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, tpx, tpy, clutx, cluty, depth);
            gpu_render_textured_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, tpx, tpy, clutx, cluty, depth);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_2d(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 8;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            uint32_t texp = gpu->buf[4] >> 16;
            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->pal = gpu->buf[2] >> 16;
            gpu->v0.tx = gpu->buf[2] & 0xff;
            gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
            gpu->v1.tx = gpu->buf[4] & 0xff;
            gpu->v1.ty = (gpu->buf[4] >> 8) & 0xff;
            gpu->v2.tx = gpu->buf[6] & 0xff;
            gpu->v2.ty = (gpu->buf[6] >> 8) & 0xff;
            gpu->v3.tx = gpu->buf[8] & 0xff;
            gpu->v3.ty = (gpu->buf[8] >> 8) & 0xff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v1.x = gpu->buf[3] & 0xffff;
            gpu->v1.y = gpu->buf[3] >> 16;
            gpu->v2.x = gpu->buf[5] & 0xffff;
            gpu->v2.y = gpu->buf[5] >> 16;
            gpu->v3.x = gpu->buf[7] & 0xffff;
            gpu->v3.y = gpu->buf[7] >> 16;

            uint16_t clutx = (gpu->pal & 0x3f) << 4;
            uint16_t cluty = (gpu->pal >> 6) & 0x1ff;
            uint16_t tpx = (texp & 0xf) << 6;
            uint16_t tpy = (texp & 0x10) << 4;
            uint16_t depth = (texp >> 7) & 0x3;

            gpu_render_textured_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, tpx, tpy, clutx, cluty, depth);
            gpu_render_textured_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, tpx, tpy, clutx, cluty, depth);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

void gpu_cmd_64(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 3;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v0.tx = gpu->buf[2] & 0xff;
            gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
            gpu->pal = gpu->buf[2] >> 16;

            uint32_t w = gpu->buf[3] & 0xffff;
            uint32_t h = gpu->buf[3] >> 16;
            uint16_t clutx = (gpu->pal & 0x3f) << 4;
            uint16_t cluty = (gpu->pal >> 6) & 0x1ff;

            gpu_render_textured_rectangle(gpu, gpu->v0, w, h, clutx, cluty, gpu->color);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

void gpu_cmd_7c(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 2;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v0.tx = gpu->buf[2] & 0xff;
            gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
            gpu->pal = gpu->buf[2] >> 16;

            uint32_t w = 16;
            uint32_t h = 16;
            uint16_t clutx = (gpu->pal & 0x3f) << 4;
            uint16_t cluty = (gpu->pal >> 6) & 0x1ff;

            gpu_render_textured_rectangle(gpu, gpu->v0, w, h, clutx, cluty, gpu->color);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

void gpu_cmd_74(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 2;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v0.tx = gpu->buf[2] & 0xff;
            gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
            gpu->pal = gpu->buf[2] >> 16;

            uint32_t w = 8;
            uint32_t h = 8;
            uint16_t clutx = (gpu->pal & 0x3f) << 4;
            uint16_t cluty = (gpu->pal >> 6) & 0x1ff;

            gpu_render_textured_rectangle(gpu, gpu->v0, w, h, clutx, cluty, rgb888_to_bgr555(gpu->color));

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

void gpu_cmd_60(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 2;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->xsiz = gpu->buf[2] & 0xffff;
            gpu->ysiz = gpu->buf[2] >> 16;

            gpu->v0.x += gpu->off_x;
            gpu->v0.y += gpu->off_y;

            gpu_render_flat_rectangle(gpu, gpu->v0, gpu->xsiz, gpu->ysiz, rgb888_to_bgr555(gpu->color));

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

void gpu_cmd_68(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 1;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;

            gpu->v0.x += gpu->off_x;
            gpu->v0.y += gpu->off_y;

            gpu->vram[PSX_VRAM_AT(gpu->v0.x, gpu->v0.y)] = rgb888_to_bgr555(gpu->color);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

void gpu_cmd_40(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 2;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->v1.x = gpu->buf[2] & 0xffff;
            gpu->v1.y = gpu->buf[2] >> 16;

            gpu_render_flat_line(gpu, gpu->v0, gpu->v1, rgb888_to_bgr555(gpu->color));

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_cmd_c0(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 2;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->c0_xcnt = 0;
            gpu->c0_ycnt = 0;
            uint32_t c0_xpos = gpu->buf[1] & 0xffff;
            uint32_t c0_ypos = gpu->buf[1] >> 16;
            gpu->c0_xsiz = gpu->buf[2] & 0xffff;
            gpu->c0_ysiz = gpu->buf[2] >> 16;
            c0_xpos = c0_xpos & 0x3ff;
            c0_ypos = c0_ypos & 0x1ff;
            gpu->c0_xsiz = ((gpu->c0_xsiz - 1) & 0x3ff) + 1;
            gpu->c0_ysiz = ((gpu->c0_ysiz - 1) & 0x1ff) + 1;
            gpu_field_read(gpu, c0_xpos, c0_ypos, c0_xpos + (uint32_t)gpu->c0_xsiz, c0_ypos + (uint32_t)gpu->c0_ysiz);
            PSX_RASTER_SYNC();
            PSX_VRAM_CPU_READ(gpu, c0_ypos, (uint32_t)gpu->c0_ysiz);
#if PSX_GPU_EXTERNAL_RASTER
            gpu_img_rects(gpu);
#endif

            gpu->c0_tsiz = ((gpu->c0_xsiz * gpu->c0_ysiz) + 1) & 0xfffffffe;
            gpu->c0_addr = c0_xpos + (c0_ypos * 1024);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_cmd_02(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 2;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            gpu->color = gpu->buf[0] & 0xffffff;
            gpu->v0.x = gpu->buf[1] & 0xffff;
            gpu->v0.y = gpu->buf[1] >> 16;
            gpu->xsiz = gpu->buf[2] & 0xffff;
            gpu->ysiz = gpu->buf[2] >> 16;

            gpu->v0.x = (gpu->v0.x & 0x3f0);
            gpu->v0.y = gpu->v0.y & 0x1ff;
            gpu->xsiz = (((gpu->xsiz & 0x3ff) + 0x0f) & 0xfffffff0);
            gpu->ysiz = gpu->ysiz & 0x1ff;

            /* the field nobody sees (gpu_update_hidden): inside the display
               window a fill draws only into it */
            if (gpu->field_hidden &&
                gpu_rect_inside_window(gpu, gpu->v0.x, gpu->v0.y, gpu->v0.x + gpu->xsiz, gpu->v0.y + gpu->ysiz))
            {
                gpu->state = GPU_STATE_RECV_CMD;

                return;
            }

#if PSX_GPU_EXTERNAL_RASTER
            if (psx_raster_fill(gpu, (int)gpu->v0.x, (int)gpu->v0.y, (int)gpu->xsiz, (int)gpu->ysiz,
                                gpu->color))
            {
                gpu_touch(gpu, gpu->v0.x, gpu->v0.y, gpu->v0.x + gpu->xsiz, gpu->v0.y + gpu->ysiz);
                gpu->state = GPU_STATE_RECV_CMD;

                return;
            }
#endif

            const uint16_t color = rgb888_to_bgr555(gpu->color);

            /* what lies inside VRAM, a row at a time */
            const int x0 = gpu->v0.x;
            const int x1 = ((x0 + (int)gpu->xsiz) < 1024) ? (x0 + (int)gpu->xsiz) : 1024;
            const int y1 = ((gpu->v0.y + (int)gpu->ysiz) < 512) ? (gpu->v0.y + (int)gpu->ysiz) : 512;

            gpu_reader_guard((uint32_t)gpu->v0.y, (uint32_t)y1);

            for (int y = gpu->v0.y; y < y1; y++)
            {
                /* the field on screen stays as it is, as for a polygon */
                if ((y & 1) == gpu->skip_rows)
                    continue;

                gpu_fill_span(&gpu->vram[PSX_VRAM_AT(x0, y)], color, x1 - x0);
            }

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_cmd_80(psx_gpu_t *gpu)
{
    switch (gpu->state)
    {
    case GPU_STATE_RECV_CMD:
    {
        gpu->state = GPU_STATE_RECV_ARGS;
        gpu->cmd_args_remaining = 3;
    }
    break;

    case GPU_STATE_RECV_ARGS:
    {
        if (!gpu->cmd_args_remaining)
        {
            gpu->state = GPU_STATE_RECV_DATA;

            PSX_RASTER_SYNC();

            uint32_t srcx = gpu->buf[1] & 0xffff;
            uint32_t srcy = gpu->buf[1] >> 16;
            uint32_t dstx = gpu->buf[2] & 0xffff;
            uint32_t dsty = gpu->buf[2] >> 16;
            uint32_t xsiz = gpu->buf[3] & 0xffff;
            uint32_t ysiz = gpu->buf[3] >> 16;

            if (xsiz && ysiz)
                gpu_field_read(gpu, srcx, srcy, srcx + xsiz, srcy + ysiz);

            /* The last solid fill (fill_key, gpu_render_rect) still holds when no pixel of its
               rectangle can change: the copy writes outside it, or copies from inside it (the fill
               colour onto the fill colour - Atlantis copies a 2x1 block onto itself between its two
               clears). Only for a fill of every row. */
            if (gpu->fill_key)
            {
                const int all_rows = !((gpu->fill_key >> 16) & 3u);
                const int dst_out = (dstx >= (uint32_t)gpu->fill_x1) || ((dstx + xsiz) <= (uint32_t)gpu->fill_x0) ||
                                    (dsty >= (uint32_t)gpu->fill_y1) || ((dsty + ysiz) <= (uint32_t)gpu->fill_y0);
                const int src_in = (srcx >= (uint32_t)gpu->fill_x0) && ((srcx + xsiz) <= (uint32_t)gpu->fill_x1) &&
                                   (srcy >= (uint32_t)gpu->fill_y0) && ((srcy + ysiz) <= (uint32_t)gpu->fill_y1);

                if (!all_rows || !(dst_out || src_in))
                    gpu->fill_key = 0u;
            }

            PSX_VRAM_CPU_READ(gpu, srcy & 0x1ffu, ysiz);

            if (dsty < 512u)
                gpu_reader_guard(dsty, ((dsty + ysiz) < 512u) ? (dsty + ysiz) : 512u);

            for (int y = 0; y < ysiz; y++)
            {
                for (int x = 0; x < xsiz; x++)
                {
                    int dstb = ((dstx + x) < 1024) && ((dsty + y) < 512);
                    int srcb = ((srcx + x) < 1024) && ((srcy + y) < 512);

                    if (dstb && srcb)
                        gpu->vram[PSX_VRAM_AT(dstx + x, dsty + y)] = gpu->vram[PSX_VRAM_AT(srcx + x, srcy + y)];
                }
            }

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

/* GP0(E1h..E6h): the drawing environment. One word each, and the only GP0
   commands whose effect the game can read back (GPUSTAT, GP1(10h)), so they
   are applied here even when the drawing itself is done on the other board. */
static PSX_GPU_HOT void gpu_env_cmd(psx_gpu_t *gpu, uint32_t w)
{
    switch (w >> 24)
    {
    case 0xe1:
    {
        gpu->gpustat &= 0xfffff800;
        gpu->gpustat |= w & 0x7ff;
        gpu->texp_x = (gpu->gpustat & 0xf) << 6;
        gpu->texp_y = (gpu->gpustat & 0x10) << 4;
        gpu->texp_d = (gpu->gpustat >> 7) & 0x3;

        gpu_update_field(gpu);
    }
    break;
    case 0xe2:
    {
        gpu->texw_mx = ((w >> 0) & 0x1f) << 3;
        gpu->texw_my = ((w >> 5) & 0x1f) << 3;
        gpu->texw_ox = ((w >> 10) & 0x1f) << 3;
        gpu->texw_oy = ((w >> 15) & 0x1f) << 3;
    }
    break;
    case 0xe3:
    {
        gpu->draw_x1 = (w >> 0) & 0x3ff;
        gpu->draw_y1 = (w >> 10) & 0x1ff;

        gpu_update_draw_visible(gpu);
    }
    break;
    case 0xe4:
    {
        gpu->draw_x2 = (w >> 0) & 0x3ff;
        gpu->draw_y2 = (w >> 10) & 0x1ff;

        gpu_update_draw_visible(gpu);
    }
    break;
    case 0xe5:
    {
        gpu->off_x = ((int32_t)(((w >> 0) & 0x7ff) << 21)) >> 21;
        gpu->off_y = ((int32_t)(((w >> 11) & 0x7ff) << 21)) >> 21;
    }
    break;
    case 0xe6:
    {
        /* To-do: Implement mask bit thing */
    }
    break;
    default:
        break;
    }
}

PSX_GPU_HOT void psx_gpu_update_cmd(psx_gpu_t *gpu)
{
    PROF_T0(t_gp0);
    PROF_INC(gp0cmds);
    gpu_note_cmd(gpu);
    psx_gpu_update_cmd_impl(gpu);
    PROF_ADD(gp0, t_gp0);
}

static PSX_GPU_HOT void psx_gpu_update_cmd_impl(psx_gpu_t *gpu)
{
    int type = (gpu->buf[0] >> 29) & 7;

    /* whatever writes VRAM ends the last solid fill's say over it (fill_key; rectangles keep it
       themselves, gpu_render_rect) */
    switch (type)
    {
    case 1:
        gpu->fill_key = 0u;
        gpu_poly(gpu);
        return;
    case 2:
        gpu->fill_key = 0u;
        gpu_reader_guard((uint32_t)gpu->draw_ry1, (uint32_t)gpu->draw_ry2 + 1u);
        gpu_line(gpu);
        return;
    case 3:
        gpu_rect(gpu);
        return;
    }

    switch (gpu->buf[0] >> 24)
    {
    case 0x00: /* nop */
        break;
    case 0x01: /* Cache clear */
        break;
    case 0x02:
        gpu->fill_key = 0u;
        gpu_cmd_02(gpu);
        break;
    /* 20h..7Fh (polygons, lines, rectangles) were handled above by type; the
       gpu_cmd_24 .. gpu_cmd_7c functions below them are no longer called - and no
       longer in OCRAM: the linker drops them */
    case 0x80:
        gpu_cmd_80(gpu); /* keeps fill_key itself */
        break;
    case 0xa0:
        gpu->fill_key = 0u;
        gpu_cmd_a0(gpu);
        break;
    case 0xc0:
        gpu_cmd_c0(gpu);
        break;
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xe4:
    case 0xe5:
    case 0xe6:
        gpu_env_cmd(gpu, gpu->buf[0]);
        break;
    default:
    {
    }
    break;
    }
}

PSX_GPU_HOT void psx_gpu_write32(psx_gpu_t *gpu, uint32_t offset, uint32_t value)
{
    switch (offset)
    {
    // GP0
    case 0x00:
    {
#if PSXE_GPU_REMOTE
        /* only the GP0 stream mode looks at the words here: its follower has to
           know where the commands are, and the word goes to the GPU board, with
           the environment commands applied here as well. The hybrid sends
           finished pictures instead, so nothing of this runs. */
        if (g_gpu_stream)
        {
            const uint32_t env = gpu_remote_gp0(gpu, value);

            if (g_gpu_remote)
            {
                if (env)
                    gpu_env_cmd(gpu, value);

                return;
            }
        }
#endif

        switch (gpu->state)
        {
        case GPU_STATE_RECV_CMD:
        {
            gpu->buf_index = 0;
            gpu->buf[gpu->buf_index++] = value;

            psx_gpu_update_cmd(gpu);
        }
        break;

        case GPU_STATE_RECV_ARGS:
        {
            gpu->buf[gpu->buf_index++] = value;
            gpu->cmd_args_remaining--;

            psx_gpu_update_cmd(gpu);
        }
        break;

        case GPU_STATE_RECV_DATA:
        {
            gpu->recv_data = value;

            psx_gpu_update_cmd(gpu);
        }
        break;
        }

        return;
    }
    break;

    // GP1
    case 0x04:
    {
        uint8_t cmd = value >> 24;

#if PSXE_GPU_REMOTE
        gpu_remote_gp1(gpu, value);
#endif

        switch (cmd)
        {
        /* What follows decides which part of VRAM is on screen and how, so a
           change here is a new picture even though no pixel was written - this
           is the buffer flip. Games rewrite these registers every frame with
           the values they already hold, which is not a change. */

        // Reset
        case 0x00:
        {
            gpu_dirty_all(gpu);
        }
        break;

        // Display enable
        case 0x03:
        {
            const uint32_t before = gpu->gpustat;

            gpu->gpustat &= ~0x00800000;
            gpu->gpustat |= (value << 23) & 0x00800000;

            if (gpu->gpustat != before)
                gpu_dirty_all(gpu);
        }
        break;
        case 0x04:
        {
        }
        break;
        case 0x05:
        {
            const uint32_t x = value & 0x3ff;
            const uint32_t y = (value >> 10) & 0x1ff;

            if ((x != gpu->disp_x) || (y != gpu->disp_y))
            {
                gpu_dirty_all(gpu);
                PROF_INC(dirty_flip);
            }

            gpu->disp_x = x;
            gpu->disp_y = y;

            gpu_update_draw_visible(gpu);
            gpu_update_field(gpu);
        }
        break;
        case 0x06:
        {
            const uint32_t x1 = value & 0xfff;
            const uint32_t x2 = (value >> 12) & 0xfff;

            if ((x1 != gpu->disp_x1) || (x2 != gpu->disp_x2))
                gpu_dirty_all(gpu);

            gpu->disp_x1 = x1;
            gpu->disp_x2 = x2;
        }
        break;
        case 0x07:
        {
            const uint32_t y1 = value & 0x1ff;
            const uint32_t y2 = (value >> 10) & 0x1ff;

            if ((y1 != gpu->disp_y1) || (y2 != gpu->disp_y2))
                gpu_dirty_all(gpu);

            gpu->disp_y1 = y1;
            gpu->disp_y2 = y2;
        }
        break;
        case 0x08:
            if (gpu->display_mode != (value & 0xffffff))
                gpu_dirty_all(gpu);

            gpu->display_mode = value & 0xffffff;

            gpu_update_draw_visible(gpu);
            gpu_update_field(gpu);

            if (gpu->event_cb_table[GPU_EVENT_DMODE])
                gpu->event_cb_table[GPU_EVENT_DMODE](gpu);
            break;

        case 0x10:
        {
            gpu->gp1_10h_req = value & 7;
        }
        break;
        }

        // log_error("GP1(%02Xh) args=%06x", value >> 24, value & 0xffffff);

        return;
    }
    break;
    }

    log_warn("Unhandled 32-bit GPU write at offset %08x (%08x)", offset, value);
}

PSX_GPU_HOT void psx_gpu_write16(psx_gpu_t *gpu, uint32_t offset, uint16_t value)
{
    PRINTF("Unhandled 16-bit GPU write at offset %08x (%04x)\r\n", offset, value);
}

PSX_GPU_HOT void psx_gpu_write8(psx_gpu_t *gpu, uint32_t offset, uint8_t value)
{
    PRINTF("Unhandled 8-bit GPU write at offset %08x (%02x)\r\n", offset, value);
}

void psx_gpu_set_event_callback(psx_gpu_t *gpu, int event, psx_gpu_event_callback_t cb)
{
    gpu->event_cb_table[event] = cb;
}

void psx_gpu_set_udata(psx_gpu_t *gpu, int index, void *udata)
{
    gpu->udata[index] = udata;
}



void psx_gpu_set_band(psx_gpu_t *gpu, int32_t share, int32_t top)
{
    if (share < 0)
        share = 0;

    if (share > 256)
        share = 256;

    gpu->band_share = share;
    gpu->band_top = top ? 1 : 0;

    gpu_update_draw_visible(gpu);
}

void psx_gpu_set_field(psx_gpu_t *gpu, uint32_t field)
{
    const int interlaced_480 = (gpu->display_mode & 0x24u) == 0x24u;

    gpu->field = interlaced_480 ? (int32_t)(field & 1u) : 0;

    gpu_update_field(gpu);
}

PSX_GPU_HOT void gpu_hblank_event(psx_gpu_t *gpu)
{
    const int interlaced_480 = (gpu->display_mode & 0x24u) == 0x24u;

    /* GPUSTAT.31, the parity of the VRAM row being sent to the screen: in 480
       line interlaced mode that of the field on screen, otherwise it changes
       with every line; 0 during the vertical blank */
    uint32_t odd = 0;

    if (gpu->line < GPU_SCANS_PER_VDRAW_NTSC)
        odd = interlaced_480 ? ((gpu->disp_y + (uint32_t)gpu->field) & 1u) : ((uint32_t)gpu->line & 1u);

    gpu->gpustat = (gpu->gpustat & 0x7fffffffu) | (odd << 31);

    gpu->line++;

    if (gpu->line == GPU_SCANS_PER_VDRAW_NTSC)
    {
        /* the other field goes on screen - before the game hears of the blank,
           so that what it draws next lands in the field that is not */
        gpu->field = interlaced_480 ? (gpu->field ^ 1) : 0;

        gpu_update_field(gpu);

#if PSXE_GPU_REMOTE
        gpu_remote_vblank(gpu, (uint32_t)gpu->field);
#endif

        if (gpu->event_cb_table[GPU_EVENT_VBLANK])
            gpu->event_cb_table[GPU_EVENT_VBLANK](gpu);

        psx_ic_irq(gpu->ic, IC_VBLANK);
    }
    else if (gpu->line == GPU_SCANS_PER_FRAME_NTSC)
    {
        if (gpu->event_cb_table[GPU_EVENT_VBLANK_END])
            gpu->event_cb_table[GPU_EVENT_VBLANK_END](gpu);

        gpu->line = 0;
    }
}

void PSX_GPU_ITC psx_gpu_update(psx_gpu_t *gpu, int cyc)
{
    const uint32_t curr = gpu->cycles_fp + (uint32_t)cyc * GPU_FP_RATIO;

    gpu->cycles_fp = curr;

    /* Nothing can happen before the next hblank edge - one compare per call */
    if (curr < gpu->next_edge_fp)
        return;

    if (!gpu->in_hblank)
    {
        gpu->in_hblank = 1;
        gpu->next_edge_fp = GPU_FP_SCANL_NTSC + 1u;

        if (gpu->event_cb_table[GPU_EVENT_HBLANK])
            gpu->event_cb_table[GPU_EVENT_HBLANK](gpu);

        gpu_hblank_event(gpu);
    }
    else
    {
        gpu->in_hblank = 0;
        gpu->next_edge_fp = GPU_FP_HDRAW_NTSC;

        if (gpu->event_cb_table[GPU_EVENT_HBLANK_END])
            gpu->event_cb_table[GPU_EVENT_HBLANK_END](gpu);

        gpu->cycles_fp = curr - GPU_FP_SCANL_NTSC;
    }
}

/* CPU cycles until psx_gpu_update has something to do: the next hblank edge */
uint32_t PSX_GPU_ITC psx_gpu_cycles_to_edge(const psx_gpu_t *gpu)
{
    if (gpu->cycles_fp >= gpu->next_edge_fp)
        return 0;

    return ((gpu->next_edge_fp - gpu->cycles_fp) / GPU_FP_RATIO) + 1u;
}

void *psx_gpu_get_display_buffer(psx_gpu_t *gpu)
{
    if (gpu->gpustat & 0x800000)
        return gpu->empty;

    return gpu->vram + PSX_VRAM_AT(gpu->disp_x, gpu->disp_y);
}

void psx_gpu_destroy(psx_gpu_t *gpu)
{
    psx_gpu_free(gpu->vram);
    psx_gpu_free(gpu->empty);
}
