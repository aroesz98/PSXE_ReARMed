#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gpu.h"
#include "log.h"
#include "fixed_math.h"
#include "fsl_debug_console.h"
#include "../prof.h"

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
#define GPU_FP_RATIO ((uint32_t)((PSX_GPU_CLOCK_FREQ_NTSC / PSX_CPU_FREQ) * 65536.0f))

#define PSX_GPU_HOT __attribute__((section(".ramfunc.$SRAM_OC")))

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

static inline uint16_t rgb888_to_rgb565(uint32_t color)
{
    uint16_t bgr = ((color & 0x0000f8) >> 3) | ((color & 0x00f800) >> 6) | ((color & 0xf80000) >> 9);

    uint16_t b = (bgr >> 10) & 0x1F; // Blue: bits 14-10
    uint16_t g = (bgr >> 5) & 0x1F;  // Green: bits 9-5
    uint16_t r = (bgr >> 0) & 0x1F;  // Red: bits 4-0
    return (r << 11) | (g << 6) | b;
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
static psx_gpu_t __attribute__((section(".bss.$SRAM_DTC"), aligned(8))) g_gpu_instance;

psx_gpu_t *psx_gpu_create(void)
{
    memset(&g_gpu_instance, 0, sizeof(g_gpu_instance));

    return &g_gpu_instance;
}

static PSX_GPU_HOT void psx_gpu_update_cmd_impl(psx_gpu_t *gpu);

void psx_gpu_init(psx_gpu_t *gpu, psx_ic_t *ic)
{
    memset(gpu, 0, sizeof(psx_gpu_t));

    gpu->io_base = PSX_GPU_BEGIN;
    gpu->io_size = PSX_GPU_SIZE;

    gpu->vram = (uint16_t *)malloc(PSX_GPU_VRAM_SIZE);
    gpu->empty = malloc(PSX_GPU_VRAM_SIZE);

    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    memset(gpu->empty, 0, PSX_GPU_VRAM_SIZE);

    gpu->state = GPU_STATE_RECV_CMD;
    gpu->gpustat |= 0x800000;

    gpu->in_hblank = 0;
    gpu->next_edge_fp = GPU_FP_HDRAW_NTSC;

    // Default window size, this is not normally needed
    gpu->display_mode = 1;

    gpu->ic = ic;
}

PSX_GPU_HOT uint32_t psx_gpu_read32(psx_gpu_t *gpu, uint32_t offset)
{
    switch (offset)
    {
    case 0x00:
    {
        uint32_t data = 0x0;

        if (gpu->c0_tsiz)
        {
            data |= gpu->vram[gpu->c0_addr + (gpu->c0_xcnt + (gpu->c0_ycnt * 1024))];

            gpu->c0_xcnt += 1;

            if (gpu->c0_xcnt == gpu->c0_xsiz)
            {
                gpu->c0_ycnt += 1;
                gpu->c0_xcnt = 0;
            }

            data |= gpu->vram[gpu->c0_addr + (gpu->c0_xcnt + (gpu->c0_ycnt * 1024))] << 16;

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
static uint16_t __attribute__((section(".bss.$SRAM_DTC"), aligned(4))) g_clut_stage[256];

static inline const uint16_t *gpu_clut_ptr(psx_gpu_t *gpu, int depth, int clutx, int cluty, uint32_t area)
{
    const uint16_t *src = &gpu->vram[clutx + (cluty << 10)];

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
        uint16_t texel = gpu->vram[(tpx + (tx >> 2)) + ((tpy + ty) * 1024)];

        int index = (texel >> ((tx & 0x3) << 2)) & 0xf;

        return clut[index];
    }
    break;

    // 8-bit
    case 1:
    {
        uint16_t texel = gpu->vram[(tpx + (tx >> 1)) + ((tpy + ty) * 1024)];

        int index = (texel >> ((tx & 0x1) << 3)) & 0xff;

        return clut[index];
    }
    break;

    // 15-bit
    default:
    {
        return gpu->vram[(tpx + tx) + ((tpy + ty) * 1024)];
    }
    break;
    }
}

PSX_GPU_HOT uint16_t gpu_fetch_texel_bilinear(psx_gpu_t *gpu, float tx, float ty, uint32_t tpx, uint32_t tpy, const uint16_t *clut, int depth)
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

__attribute__((always_inline)) static inline uint16_t gpu_blend_rgb565(uint16_t src, uint16_t dst, int transp_mode)
{
    int cr = (((src >> 11) & 0x1f) << 3) << 8;
    int cg = (((src >> 5) & 0x3f) << 2) << 8;
    int cb = (((src >> 0) & 0x1f) << 3) << 8;
    const int br = (((dst >> 11) & 0x1f) << 3) << 8;
    const int bg = (((dst >> 5) & 0x3f) << 2) << 8;
    const int bb = (((dst >> 0) & 0x1f) << 3) << 8;

    switch (transp_mode)
    {
    case 0:  // 0.5*B + 0.5*F
        cr = (br * 128 + cr * 128) >> 8;
        cg = (bg * 128 + cg * 128) >> 8;
        cb = (bb * 128 + cb * 128) >> 8;
        break;
    case 1:  // 1.0*B + 1.0*F
        cr = (br + cr) >> 8;
        cg = (bg + cg) >> 8;
        cb = (bb + cb) >> 8;
        break;
    case 2:  // 1.0*B - 1.0*F
        cr = (br - cr) >> 8;
        cg = (bg - cg) >> 8;
        cb = (bb - cb) >> 8;
        break;
    case 3:  // 1.0*B + 0.25*F
        cr = (br + (cr * 64)) >> 8;
        cg = (bg + (cg * 64)) >> 8;
        cb = (bb + (cb * 64)) >> 8;
        break;
    }

    // Saturate to 8-bit
    unsigned int ucr = (cr < 0) ? 0 : (cr > 255) ? 255 : cr;
    unsigned int ucg = (cg < 0) ? 0 : (cg > 255) ? 255 : cg;
    unsigned int ucb = (cb < 0) ? 0 : (cb > 255) ? 255 : cb;

    // OPTIMIZED: Inline rgb888_to_rgb565 conversion
    const uint32_t rgb888 = ucr | (ucg << 8) | (ucb << 16);
    const uint32_t bgr = ((rgb888 & 0x0000f8) >> 3) | ((rgb888 & 0x00f800) >> 6) | ((rgb888 & 0xf80000) >> 9);
    const uint32_t b = (bgr >> 10) & 0x1F;
    const uint32_t g = (bgr >> 5) & 0x1F;
    const uint32_t r = (bgr >> 0) & 0x1F;
    return (r << 11) | (g << 6) | b;
}

//static inline uint16_t gpu_blend_rgb565(uint16_t src, uint16_t dst, int transp_mode)
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
//    return rgb888_to_rgb565(rgb);
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
    int64_t num_dx = (int64_t)attr_a * (vb->y - vc->y) +
                     (int64_t)attr_b * (vc->y - va->y) +
                     (int64_t)attr_c * (va->y - vb->y);

    int64_t num_dy = (int64_t)attr_a * (vc->x - vb->x) +
                     (int64_t)attr_b * (va->x - vc->x) +
                     (int64_t)attr_c * (vb->x - va->x);

    plane.dx = (int32_t)((num_dx << frac_bits) / area);
    plane.dy = (int32_t)((num_dy << frac_bits) / area);

    int32_t base = attr_a << frac_bits;
    int32_t x_off = xmin - va->x;
    int32_t y_off = ymin - va->y;

    plane.row = base + plane.dx * x_off + plane.dy * y_off;
    return plane;
}

#define CLAMP(v, d, u) ((v) <= (d)) ? (d) : (((v) >= (u)) ? (u) : (v))

static inline void gpu_fill_span(uint16_t *dst, uint16_t color, int count)
{
    if (count <= 0)
        return;

    if (((uintptr_t)dst & 0x2) && count)
    {
        *dst++ = color;
        --count;
    }

    uint32_t packed = (uint32_t)color | ((uint32_t)color << 16);
    uint32_t *dst32 = (uint32_t *)dst;
    while (count >= 2)
    {
        *dst32++ = packed;
        count -= 2;
    }

    if (count)
    {
        uint16_t *tail = (uint16_t *)dst32;
        *tail = color;
    }
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
//    int ymin = max(min3(a.y, b.y, c.y), gpu->draw_y1);
//    int xmax = min(max3(a.x, b.x, c.x), min(gpu->draw_x2, 1023));
//    int ymax = min(max3(a.y, b.y, c.y), min(gpu->draw_y2, 511));
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
//    const uint16_t flat_color565 = rgb888_to_rgb565(flat_color);
//    const int32_t edge0_a = edge0.a;
//    const int32_t edge1_a = edge1.a;
//    const int32_t edge2_a = edge2.a;
//    const int32_t edge0_b = edge0.b;
//    const int32_t edge1_b = edge1.b;
//    const int32_t edge2_b = edge2.b;
//
//    if (!is_textured && !is_shaded && !transparency_enabled)
//    {
//        const uint16_t out_color = flat_color565;
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
//                *dst++ = rgb888_to_rgb565(mod_color);
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
//                    const uint16_t out_color = rgb888_to_rgb565(mod_color);
//                    *dst = gpu_blend_rgb565(out_color, *dst, transp_mode);
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
//                    *dst = gpu_blend_rgb565(flat_color565, *dst, transp_mode);
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
//                        out_color = rgb888_to_rgb565(rgb);
//                    }
//
//                    if (transparency_enabled && (texel & 0x8000))
//                    {
//                        *dst = gpu_blend_rgb565(out_color, *dst, transp_mode);
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
//                    out_color = rgb888_to_rgb565(rgb);
//                }
//
//                if (transparency_enabled && (texel & 0x8000))
//                {
//                    *dst = gpu_blend_rgb565(out_color, *dst, transp_mode);
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
__attribute__((always_inline)) static inline uint16_t bgr555_to_rgb565(uint16_t texel)
{
    return (uint16_t)(((texel & 0x1fu) << 11) | (((texel >> 5) & 0x1fu) << 6) | ((texel >> 10) & 0x1fu));
}

__attribute__((always_inline)) static inline uint16_t modulate_rgb565(uint16_t texel, uint8_t mod_r, uint8_t mod_g, uint8_t mod_b)
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

    // OPTIMIZED: Inline rgb888_to_rgb565 conversion to eliminate function call
    const uint32_t rgb888 = pr | (pg << 8) | (pb << 16);
    const uint32_t bgr = ((rgb888 & 0x0000f8) >> 3) | ((rgb888 & 0x00f800) >> 6) | ((rgb888 & 0xf80000) >> 9);
    const uint32_t b = (bgr >> 10) & 0x1F;
    const uint32_t g = (bgr >> 5) & 0x1F;
    const uint32_t r = (bgr >> 0) & 0x1F;
    return (r << 11) | (g << 6) | b;
}

PSX_GPU_HOT void gpu_render_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge)
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
        return;
    int32_t area = (int32_t)area64;

    int xmin = max(min3(a.x, b.x, c.x), gpu->draw_x1);
    int ymin = max(min3(a.y, b.y, c.y), gpu->draw_y1);
    int xmax = min(max3(a.x, b.x, c.x), min(gpu->draw_x2, 1023));
    int ymax = min(max3(a.y, b.y, c.y), min(gpu->draw_y2, 511));

    if (xmin > xmax || ymin > ymax)
        return;

#if PSX_PROFILE
    {
        const uint32_t bbox = (uint32_t)(xmax - xmin + 1) * (uint32_t)(ymax - ymin + 1);
        const int prof_depth = (data.texp >> 7) & 3;

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

    int32_t e0_row = edge_eval(&edge0, xmin, ymin);
    int32_t e1_row = edge_eval(&edge1, xmin, ymin);
    int32_t e2_row = edge_eval(&edge2, xmin, ymin);

    uint16_t *vram = gpu->vram;
    const uint32_t flat_color = data.v[0].c;
    // Inline rgb888_to_rgb565 conversion - EXACT match to original
    const uint16_t bgr_flat = ((flat_color & 0x0000f8) >> 3) | ((flat_color & 0x00f800) >> 6) | ((flat_color & 0xf80000) >> 9);
    const uint16_t flat_color565 = (((bgr_flat >> 0) & 0x1F) << 11) | (((bgr_flat >> 5) & 0x1F) << 6) | ((bgr_flat >> 10) & 0x1F);
    const int32_t edge0_a = edge0.a;
    const int32_t edge1_a = edge1.a;
    const int32_t edge2_a = edge2.a;
    const int32_t edge0_b = edge0.b;
    const int32_t edge1_b = edge1.b;
    const int32_t edge2_b = edge2.b;

    // ==== FAST PATH: Flat untextured non-transparent ====
    // Matches original implementation exactly for best compatibility
    if (!is_textured && !is_shaded && !transparency_enabled)
    {
        const uint16_t out_color = flat_color565;
        int vram_row = ymin * 1024;

        for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
        {
            int32_t e0 = e0_row;
            int32_t e1 = e1_row;
            int32_t e2 = e2_row;
            int x = xmin;

            while (x <= xmax && (e0 | e1 | e2) < 0)
            {
                e0 += edge0_a;
                e1 += edge1_a;
                e2 += edge2_a;
                ++x;
            }

            if (x <= xmax)
            {
                int start_x = x;
                int count = 0;
                do
                {
                    count++;
                    e0 += edge0_a;
                    e1 += edge1_a;
                    e2 += edge2_a;
                    ++x;
                } while (x <= xmax && (e0 | e1 | e2) >= 0);
                gpu_fill_span(&vram[vram_row + start_x], out_color, count);
            }

            e0_row += edge0_b;
            e1_row += edge1_b;
            e2_row += edge2_b;
        }
        return;
    }

    // ==== FAST PATH: Flat untextured (transparent or needs blending) ====
    if (!is_textured && !is_shaded)
    {
        const int transp_mode = (gpu->gpustat >> 5) & 3;
        const uint16_t src_color = flat_color565;

        // Pre-compute blend factors for src_color to avoid repeated calculation
        const int cr_base = (((src_color >> 11) & 0x1f) << 3) << 8;
        const int cg_base = (((src_color >> 5) & 0x3f) << 2) << 8;
        const int cb_base = (((src_color >> 0) & 0x1f) << 3) << 8;

        int vram_row = ymin * 1024;

        for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
        {
            int32_t e0 = e0_row;
            int32_t e1 = e1_row;
            int32_t e2 = e2_row;
            uint16_t *__restrict dst = &vram[vram_row + xmin];
            int x = xmin;

            // Skip pixels outside triangle
            while (x <= xmax && (e0 | e1 | e2) < 0)
            {
                e0 += edge0_a;
                e1 += edge1_a;
                e2 += edge2_a;
                ++x;
                ++dst;
            }

            // Blend pixels inside triangle with pre-computed factors
            while (x <= xmax && (e0 | e1 | e2) >= 0)
            {
                // Inline blend with pre-computed src values
                const uint16_t dst_val = *dst;
                const int br = (((dst_val >> 11) & 0x1f) << 3) << 8;
                const int bg = (((dst_val >> 5) & 0x3f) << 2) << 8;
                const int bb = (((dst_val >> 0) & 0x1f) << 3) << 8;

                int cr, cg, cb;
                if (transp_mode == 0) {
                    cr = (br * 128 + cr_base * 128) >> 8;
                    cg = (bg * 128 + cg_base * 128) >> 8;
                    cb = (bb * 128 + cb_base * 128) >> 8;
                } else if (transp_mode == 1) {
                    cr = (br + cr_base) >> 8;
                    cg = (bg + cg_base) >> 8;
                    cb = (bb + cb_base) >> 8;
                } else if (transp_mode == 2) {
                    cr = (br - cr_base) >> 8;
                    cg = (bg - cg_base) >> 8;
                    cb = (bb - cb_base) >> 8;
                } else {
                    cr = (br + cr_base * 64) >> 8;
                    cg = (bg + cg_base * 64) >> 8;
                    cb = (bb + cb_base * 64) >> 8;
                }

                if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
                if (cg < 0) cg = 0; else if (cg > 255) cg = 255;
                if (cb < 0) cb = 0; else if (cb > 255) cb = 255;

                *dst = ((cr >> 3) << 11) | ((cg >> 2) << 5) | (cb >> 3);

                e0 += edge0_a;
                e1 += edge1_a;
                e2 += edge2_a;
                ++x;
                ++dst;
            }

            e0_row += edge0_b;
            e1_row += edge1_b;
            e2_row += edge2_b;
        }
        return;
    }

    plane_attr_t r_plane = {0}, g_plane = {0}, b_plane = {0};
    plane_attr_t tx_plane = {0}, ty_plane = {0};
    int32_t r_row = 0, g_row = 0, b_row = 0;
    int32_t tx_row = 0, ty_row = 0;

    if (is_shaded)
    {
        r_plane = plane_setup(&a, &b, &c, area, (a.c >> 0) & 0xff, (b.c >> 0) & 0xff, (c.c >> 0) & 0xff, ATTR_FRAC_BITS, xmin, ymin);
        g_plane = plane_setup(&a, &b, &c, area, (a.c >> 8) & 0xff, (b.c >> 8) & 0xff, (c.c >> 8) & 0xff, ATTR_FRAC_BITS, xmin, ymin);
        b_plane = plane_setup(&a, &b, &c, area, (a.c >> 16) & 0xff, (b.c >> 16) & 0xff, (c.c >> 16) & 0xff, ATTR_FRAC_BITS, xmin, ymin);
        r_row = r_plane.row;
        g_row = g_plane.row;
        b_row = b_plane.row;
    }

    if (is_textured)
    {
        tx_plane = plane_setup(&a, &b, &c, area, a.tx, b.tx, c.tx, ATTR_FRAC_BITS, xmin, ymin);
        ty_plane = plane_setup(&a, &b, &c, area, a.ty, b.ty, c.ty, ATTR_FRAC_BITS, xmin, ymin);
        tx_row = tx_plane.row;
        ty_row = ty_plane.row;
    }

    // ==== OPTIMIZED PATH: Textured flat-shaded (most common case) ====
    if (is_textured && !is_shaded)
    {
        // Compute texture parameters only when needed
        const int tpx = (data.texp & 0xf) << 6;
        const int tpy = (data.texp & 0x10) << 4;
        const int clutx = (data.clut & 0x3f) << 4;
        const int cluty = (data.clut >> 6) & 0x1ff;
        const int depth = (data.texp >> 7) & 3;
        const int transp_mode = (data.texp >> 5) & 3;

        // Pre-compute modulation factors in 8-bit
        const uint8_t mod_r = flat_color & 0xff;
        const uint8_t mod_g = (flat_color >> 8) & 0xff;
        const uint8_t mod_b = (flat_color >> 16) & 0xff;

        // Hoist texture window masking (rarely changes per triangle).
        // Folded into a single and/or pair so the inner loop only pays 2 ops.
        const uint32_t texw_and_x = (uint32_t)(uint16_t)(~gpu->texw_mx) & 0xffu;
        const uint32_t texw_or_x = (uint32_t)(gpu->texw_ox & gpu->texw_mx) & 0xffu;
        const uint32_t texw_and_y = (uint32_t)(uint16_t)(~gpu->texw_my) & 0xffu;
        const uint32_t texw_or_y = (uint32_t)(gpu->texw_oy & gpu->texw_my) & 0xffu;

        // Palette staged in DTCM when the primitive is big enough to profit
        const uint16_t *const clut = gpu_clut_ptr(gpu, depth, clutx, cluty,
                                                  (uint32_t)(xmax - xmin + 1) * (uint32_t)(ymax - ymin + 1));

        int vram_row = ymin * 1024;

        /* ---------------------------------------------------------------
           Specialised span loop for the case games spend most of their time
           in: paletted texture (4/8 bpp), flat shading, opaque, not raw.
           Every loop invariant (depth, modulation, raw/transparency) is
           resolved at compile time here, so the inner loop is only the work
           that actually differs per pixel.
           --------------------------------------------------------------- */
#if PSX_PROFILE
        {
            const uint32_t bbox = (uint32_t)(xmax - xmin + 1) * (uint32_t)(ymax - ymin + 1);

            if (transparency_enabled)
                g_prof.px_transp += bbox;

            if (is_raw)
                g_prof.px_raw += bbox;

            if ((depth != 2) && !is_raw)
                g_prof.px_fast += bbox;
        }
#endif

        if ((depth != 2) && !is_raw)
        {
            const int32_t tx_dx = tx_plane.dx;
            const int32_t ty_dx = ty_plane.dx;
            const int32_t tx_dy = tx_plane.dy;
            const int32_t ty_dy = ty_plane.dy;
            const int neutral = (mod_r == 0x80) && (mod_g == 0x80) && (mod_b == 0x80);

#define PSXE_SPAN_TEXEL(IS4)                                                                  \
            ((IS4)                                                                             \
                 ? clut[(vram[(tpx + (tx >> 2)) + ((tpy + ty) << 10)] >> ((tx & 3u) << 2)) & 0xfu]  \
                 : clut[(vram[(tpx + (tx >> 1)) + ((tpy + ty) << 10)] >> ((tx & 1u) << 3)) & 0xffu])

#define PSXE_SPAN_LOOP(IS4, NEUT, TRANSP)                                                             \
            for (int y = ymin; y <= ymax; ++y, vram_row += 1024)                               \
            {                                                                                  \
                int32_t e0 = e0_row;                                                           \
                int32_t e1 = e1_row;                                                           \
                int32_t e2 = e2_row;                                                           \
                int32_t txv = tx_row;                                                          \
                int32_t tyv = ty_row;                                                          \
                uint16_t *__restrict dst = &vram[vram_row + xmin];                              \
                int x = xmin;                                                                  \
                                                                                               \
                while (x <= xmax && (e0 | e1 | e2) < 0)                                        \
                {                                                                              \
                    e0 += edge0_a; e1 += edge1_a; e2 += edge2_a;                                \
                    txv += tx_dx; tyv += ty_dx;                                                 \
                    ++x; ++dst;                                                                \
                }                                                                              \
                                                                                               \
                while (x <= xmax && (e0 | e1 | e2) >= 0)                                       \
                {                                                                              \
                    const uint32_t tx = (((uint32_t)txv >> ATTR_FRAC_BITS) & texw_and_x) | texw_or_x; \
                    const uint32_t ty = (((uint32_t)tyv >> ATTR_FRAC_BITS) & texw_and_y) | texw_or_y; \
                    const uint16_t texel = PSXE_SPAN_TEXEL(IS4);                               \
                                                                                               \
                    if (texel)                                                                 \
                    {                                                                          \
                        const uint16_t out = (NEUT) ? bgr555_to_rgb565(texel)                  \
                                                    : modulate_rgb565(texel, mod_r, mod_g, mod_b); \
                                                                                               \
                        *dst = ((TRANSP) && (texel & 0x8000u))                                 \
                                   ? gpu_blend_rgb565(out, *dst, transp_mode)                  \
                                   : out;                                                      \
                    }                                                                          \
                                                                                               \
                    e0 += edge0_a; e1 += edge1_a; e2 += edge2_a;                                \
                    txv += tx_dx; tyv += ty_dx;                                                 \
                    ++x; ++dst;                                                                \
                }                                                                              \
                                                                                               \
                e0_row += edge0_b; e1_row += edge1_b; e2_row += edge2_b;                        \
                tx_row += tx_dy; ty_row += ty_dy;                                               \
            }

            if (transparency_enabled)
            {
                if (depth == 0)
                {
                    if (neutral)
                    {
                        PSXE_SPAN_LOOP(1, 1, 1)
                    }
                    else
                    {
                        PSXE_SPAN_LOOP(1, 0, 1)
                    }
                }
                else
                {
                    if (neutral)
                    {
                        PSXE_SPAN_LOOP(0, 1, 1)
                    }
                    else
                    {
                        PSXE_SPAN_LOOP(0, 0, 1)
                    }
                }
            }
            else
            {
                if (depth == 0)
                {
                    if (neutral)
                    {
                        PSXE_SPAN_LOOP(1, 1, 0)
                    }
                    else
                    {
                        PSXE_SPAN_LOOP(1, 0, 0)
                    }
                }
                else
                {
                    if (neutral)
                    {
                        PSXE_SPAN_LOOP(0, 1, 0)
                    }
                    else
                    {
                        PSXE_SPAN_LOOP(0, 0, 0)
                    }
                }
            }

#undef PSXE_SPAN_LOOP
#undef PSXE_SPAN_TEXEL

            return;
        }

        for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
        {
            int32_t e0 = e0_row;
            int32_t e1 = e1_row;
            int32_t e2 = e2_row;
            int32_t tx_val = tx_row;
            int32_t ty_val = ty_row;
            uint16_t *__restrict dst = &vram[vram_row + xmin];
            int x = xmin;

            while (x <= xmax && (e0 | e1 | e2) < 0)
            {
                e0 += edge0_a;
                e1 += edge1_a;
                e2 += edge2_a;
                tx_val += tx_plane.dx;
                ty_val += ty_plane.dx;
                ++x;
                ++dst;
            }

            while (x <= xmax && (e0 | e1 | e2) >= 0)
            {
                // AGGRESSIVE OPTIMIZATION: Unroll loop for 15-bit textured non-transparent case
                // This is the most common path in games
                if (depth == 2 && !transparency_enabled && !is_raw && (xmax - x + 1) >= 2)
                {
                    // Fast path: Process 2 pixels at once with manual unrolling
                    // We only unroll by 2 to avoid edge test complications

                    // Check both pixels will be inside triangle
                    int32_t e0_next = e0 + edge0_a;
                    int32_t e1_next = e1 + edge1_a;
                    int32_t e2_next = e2 + edge2_a;

                    if ((e0_next | e1_next | e2_next) >= 0 && x + 1 <= xmax)
                    {
                        // Pixel 0
                        int tx0 = tx_val >> ATTR_FRAC_BITS;
                        int ty0 = ty_val >> ATTR_FRAC_BITS;
                        tx0 = (tx0 & texw_and_x) | texw_or_x;
                        ty0 = (ty0 & texw_and_y) | texw_or_y;
                        uint16_t texel0 = vram[(tpx + tx0) + ((tpy + ty0) << 10)];

                        // Pixel 1
                        int32_t tx_val1 = tx_val + tx_plane.dx;
                        int32_t ty_val1 = ty_val + ty_plane.dx;
                        int tx1 = tx_val1 >> ATTR_FRAC_BITS;
                        int ty1 = ty_val1 >> ATTR_FRAC_BITS;
                        tx1 = (tx1 & texw_and_x) | texw_or_x;
                        ty1 = (ty1 & texw_and_y) | texw_or_y;
                        uint16_t texel1 = vram[(tpx + tx1) + ((tpy + ty1) << 10)];

                        // Modulate both (compiler can pipeline these)
                        if (texel0 != 0) {
                            dst[0] = modulate_rgb565(texel0, mod_r, mod_g, mod_b);
                        }
                        if (texel1 != 0) {
                            dst[1] = modulate_rgb565(texel1, mod_r, mod_g, mod_b);
                        }

                        // Update for 2 pixels
                        e0 = e0_next + edge0_a;
                        e1 = e1_next + edge1_a;
                        e2 = e2_next + edge2_a;
                        tx_val = tx_val1 + tx_plane.dx;
                        ty_val = ty_val1 + ty_plane.dx;
                        dst += 2;
                        x += 2;
                        continue;
                    }
                }

                // Standard single-pixel processing
                int tx = tx_val >> ATTR_FRAC_BITS;
                int ty = ty_val >> ATTR_FRAC_BITS;

                // OPTIMIZED: Inline texture fetch for 15-bit mode
                uint16_t texel;
                if (depth == 2)  // 15-bit direct (most common)
                {
                    // Apply texture window using bitwise ops (branchless)
                    tx = (tx & texw_and_x) | texw_or_x;
                    ty = (ty & texw_and_y) | texw_or_y;

                    // OPTIMIZED: Combined calculation with masking and shifting
                    texel = vram[tpx + tx + ((tpy + ty) << 10)];
                }
                else if (depth == 1)  // 8-bit
                {
                    tx = (tx & texw_and_x) | texw_or_x;
                    ty = (ty & texw_and_y) | texw_or_y;

                    uint16_t packed = vram[(tpx + (tx >> 1)) + ((tpy + ty) << 10)];
                    int index = (packed >> ((tx & 0x1) << 3)) & 0xff;
                    texel = clut[index];
                }
                else  // 4-bit
                {
                    tx = (tx & texw_and_x) | texw_or_x;
                    ty = (ty & texw_and_y) | texw_or_y;

                    uint16_t packed = vram[(tpx + (tx >> 2)) + ((tpy + ty) << 10)];
                    int index = (packed >> ((tx & 0x3) << 2)) & 0xf;
                    texel = clut[index];
                }

                // Branchless transparency check using conditional selection
                const int is_transparent = transparency_enabled & ((texel >> 15) & 1);

                if (__builtin_expect(texel != 0, 1))
                {
                    uint16_t out_color;
                    if (is_raw)
                    {
                        out_color = texel;
                    }
                    else
                    {
                        // OPTIMIZED: SIMD modulation with DSP instructions
                        out_color = modulate_rgb565(texel, mod_r, mod_g, mod_b);
                    }

                    if (is_transparent)
                    {
                        *dst = gpu_blend_rgb565(out_color, *dst, transp_mode);
                    }
                    else
                    {
                        *dst = out_color;
                    }
                }

                // Use SIMD ADD for parallel edge/coordinate updates
                e0 += edge0_a;
                e1 += edge1_a;
                e2 += edge2_a;
                tx_val += tx_plane.dx;
                ty_val += ty_plane.dx;
                ++x;
                ++dst;
            }

            e0_row += edge0_b;
            e1_row += edge1_b;
            e2_row += edge2_b;
            tx_row += tx_plane.dy;
            ty_row += ty_plane.dy;
        }
        return;
    }

    // ==== OPTIMIZED PATH: Gouraud shading without texture ====
    if (!is_textured)
    {
        const int transp_mode = (gpu->gpustat >> 5) & 3;
        int vram_row = ymin * 1024;

        for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
        {
            int32_t e0 = e0_row;
            int32_t e1 = e1_row;
            int32_t e2 = e2_row;
            int32_t r_val = r_row;
            int32_t g_val = g_row;
            int32_t b_val = b_row;
            uint16_t *dst = &vram[vram_row + xmin];
            int x = xmin;
            const int kernel_row = ((y - ymin) & 3) << 2;
            int dx_dither = 0;

            // Skip pixels outside triangle
            while (x <= xmax && (e0 | e1 | e2) < 0)
            {
                e0 += edge0_a;
                e1 += edge1_a;
                e2 += edge2_a;
                r_val += r_plane.dx;
                g_val += g_plane.dx;
                b_val += b_plane.dx;
                ++x;
                ++dst;
                dx_dither = (dx_dither + 1) & 3;
            }

            // Render pixels inside triangle - OPTIMIZED with reduced overhead
            while (x <= xmax && (e0 | e1 | e2) >= 0)
            {
                // Extract and dither colors
                const int dither = g_psx_gpu_dither_kernel[dx_dither | kernel_row];

                // OPTIMIZED: Combine shift, add, and saturate - compiler friendly
                int cr = (r_val >> ATTR_FRAC_BITS) + dither;
                int cg = (g_val >> ATTR_FRAC_BITS) + dither;
                int cb = (b_val >> ATTR_FRAC_BITS) + dither;

                // Saturate to 8-bit
                if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
                if (cg < 0) cg = 0; else if (cg > 255) cg = 255;
                if (cb < 0) cb = 0; else if (cb > 255) cb = 255;

                // OPTIMIZED: Inline rgb888_to_rgb565 conversion - EXACT match to original
                const uint32_t rgb888 = cr | (cg << 8) | (cb << 16);
                const uint32_t bgr_tmp = ((rgb888 & 0x0000f8) >> 3) | ((rgb888 & 0x00f800) >> 6) | ((rgb888 & 0xf80000) >> 9);
                const uint16_t out_color = (((bgr_tmp >> 0) & 0x1F) << 11) | (((bgr_tmp >> 5) & 0x1F) << 6) | ((bgr_tmp >> 10) & 0x1F);

                if (transparency_enabled)
                {
                    *dst = gpu_blend_rgb565(out_color, *dst, transp_mode);
                }
                else
                {
                    *dst = out_color;
                }

                e0 += edge0_a;
                e1 += edge1_a;
                e2 += edge2_a;
                r_val += r_plane.dx;
                g_val += g_plane.dx;
                b_val += b_plane.dx;
                ++x;
                ++dst;
                dx_dither = (dx_dither + 1) & 3;
            }

            e0_row += edge0_b;
            e1_row += edge1_b;
            e2_row += edge2_b;
            r_row += r_plane.dy;
            g_row += g_plane.dy;
            b_row += b_plane.dy;
        }
        return;
    }

    // ==== Fallback path for textured+Gouraud ====
    // Compute texture parameters only when needed
    const int tpx = (data.texp & 0xf) << 6;
    const int tpy = (data.texp & 0x10) << 4;
    const int clutx = (data.clut & 0x3f) << 4;
    const int cluty = (data.clut >> 6) & 0x1ff;
    const int depth = (data.texp >> 7) & 3;
    const int transp_mode = (data.texp >> 5) & 3;
    const uint16_t *const clut = gpu_clut_ptr(gpu, depth, clutx, cluty,
                                              (uint32_t)(xmax - xmin + 1) * (uint32_t)(ymax - ymin + 1));

    int32_t r_val_row = r_row;
    int32_t g_val_row = g_row;
    int32_t b_val_row = b_row;
    int vram_row = ymin * 1024;

    for (int y = ymin; y <= ymax; ++y, vram_row += 1024)
    {
        int32_t e0 = e0_row;
        int32_t e1 = e1_row;
        int32_t e2 = e2_row;
        int32_t r_val = r_val_row;
        int32_t g_val = g_val_row;
        int32_t b_val = b_val_row;
        int32_t tx_val = tx_row;
        int32_t ty_val = ty_row;
        uint16_t *dst = &vram[vram_row + xmin];
        int x = xmin;
        const int kernel_row = ((y - ymin) & 3) << 2;
        int dx_dither = 0;

        while (x <= xmax && (e0 | e1 | e2) < 0)
        {
            e0 += edge0_a;
            e1 += edge1_a;
            e2 += edge2_a;
            r_val += r_plane.dx;
            g_val += g_plane.dx;
            b_val += b_plane.dx;
            tx_val += tx_plane.dx;
            ty_val += ty_plane.dx;
            ++x;
            ++dst;
            dx_dither = (dx_dither + 1) & 3;
        }

        while (x <= xmax && (e0 | e1 | e2) >= 0)
        {
            const int cr = r_val >> ATTR_FRAC_BITS;
            const int cg = g_val >> ATTR_FRAC_BITS;
            const int cb = b_val >> ATTR_FRAC_BITS;
            const int dither = g_psx_gpu_dither_kernel[dx_dither | kernel_row];
            const int dr = fast_saturate_u8(cr + dither);
            const int dg = fast_saturate_u8(cg + dither);
            const int db = fast_saturate_u8(cb + dither);

            const int tx = tx_val >> ATTR_FRAC_BITS;
            const int ty = ty_val >> ATTR_FRAC_BITS;
            const uint16_t texel = gpu_fetch_texel(gpu, tx, ty, tpx, tpy, clut, depth);

            if (__builtin_expect(texel != 0, 1))
            {
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
                    const int pr = (tr * dr) >> 7;
                    const int pg = (tg * dg) >> 7;
                    const int pb = (tb * db) >> 7;
                    const unsigned int upr = fast_saturate_u8(pr);
                    const unsigned int upg = fast_saturate_u8(pg);
                    const unsigned int upb = fast_saturate_u8(pb);
                    // Inline rgb888_to_rgb565 conversion
                    const uint32_t rgb888 = upr | (upg << 8) | (upb << 16);
                    const uint32_t bgr_tmp = ((rgb888 & 0x0000f8) >> 3) | ((rgb888 & 0x00f800) >> 6) | ((rgb888 & 0xf80000) >> 9);
                    out_color = (((bgr_tmp >> 0) & 0x1F) << 11) | (((bgr_tmp >> 5) & 0x1F) << 6) | ((bgr_tmp >> 10) & 0x1F);
                }

                if (transparency_enabled && (texel & 0x8000))
                {
                    *dst = gpu_blend_rgb565(out_color, *dst, transp_mode);
                }
                else
                {
                    *dst = out_color;
                }
            }

            e0 += edge0_a;
            e1 += edge1_a;
            e2 += edge2_a;
            r_val += r_plane.dx;
            g_val += g_plane.dx;
            b_val += b_plane.dx;
            tx_val += tx_plane.dx;
            ty_val += ty_plane.dx;
            ++x;
            ++dst;
            dx_dither = (dx_dither + 1) & 3;
        }

        e0_row += edge0_b;
        e1_row += edge1_b;
        e2_row += edge2_b;
        r_val_row += r_plane.dy;
        g_val_row += g_plane.dy;
        b_val_row += b_plane.dy;
        tx_row += tx_plane.dy;
        ty_row += ty_plane.dy;
    }
}


PSX_GPU_HOT void gpu_render_rect(psx_gpu_t *gpu, rect_data_t data)
{
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
    int32_t y0 = max(screen_y0, gpu->draw_y1);
    int32_t x1 = min(screen_x0 + width, gpu->draw_x2 + 1);
    int32_t y1 = min(screen_y0 + height, gpu->draw_y2 + 1);

    if (x0 >= x1 || y0 >= y1)
        return;

#if PSX_PROFILE
    g_prof.pixels += (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
    g_prof.px_rect += (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
#endif

    const int rect_w = x1 - x0;
    const uint16_t solid_color = rgb888_to_rgb565(data.v0.c);

    if (!is_textured && !base_transp)
    {
        for (int32_t y = y0; y < y1; ++y)
        {
            uint16_t *dst = &gpu->vram[x0 + y * 1024];
            gpu_fill_span(dst, solid_color, rect_w);
        }
        return;
    }

    if (!is_textured)
    {
        for (int32_t y = y0; y < y1; ++y)
        {
            uint16_t *dst = &gpu->vram[x0 + y * 1024];
            for (int32_t i = 0; i < rect_w; ++i, ++dst)
            {
                *dst = gpu_blend_rgb565(solid_color, *dst, transp_mode);
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

    for (int32_t y = y0; y < y1; ++y, ++tex_y)
    {
        uint16_t *dst = &gpu->vram[x0 + y * 1024];
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
                out_color = rgb888_to_rgb565(rgb);
            }

            if (__builtin_expect(base_transp && (texel & 0x8000), 0))
            {
                *dst = gpu_blend_rgb565(out_color, *dst, transp_mode);
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
                 (y >= gpu->draw_y1) && (y <= gpu->draw_y2);

        if ((x < 1024) && (y < 512) && (x >= 0) && (y >= 0) && bc)
            gpu->vram[x + (y * 1024)] = color;

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
                 (y >= gpu->draw_y1) && (y <= gpu->draw_y2);

        if ((x < 1024) && (y < 512) && (x >= 0) && (y >= 0) && bc)
            gpu->vram[x + (y * 1024)] = color;

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
    v0.x += gpu->off_x;
    v0.y += gpu->off_y;
    v1.x += gpu->off_x;
    v1.y += gpu->off_y;

    plotLine(gpu, v0.x, v0.y, v1.x, v1.y, color);
}

PSX_GPU_HOT void gpu_render_flat_rectangle(psx_gpu_t *gpu, vertex_t v, uint32_t w, uint32_t h, uint32_t color)
{
    /* Offset coordinates */
    v.x += gpu->off_x;
    v.y += gpu->off_y;

    /* Calculate bounding box */
    int xmin = max(v.x, gpu->draw_x1);
    int ymin = max(v.y, gpu->draw_y1);
    int xmax = min(xmin + w, gpu->draw_x2);
    int ymax = min(ymin + h, gpu->draw_y2);

    /* Early exit if clipped completely */
    if (xmin >= xmax || ymin >= ymax)
        return;

    uint32_t rect_width = xmax - xmin;
    uint16_t color16 = (uint16_t)color;

    /* Fast rectangle filling inspired by pushBlock16 approach */
    for (uint32_t y = ymin; y < ymax; y++)
    {
        uint16_t *line_ptr = &gpu->vram[xmin + (y * 1024)];
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

PSX_GPU_HOT void gpu_render_textured_rectangle(psx_gpu_t *gpu, vertex_t v, uint32_t w, uint32_t h, uint16_t clutx, uint16_t cluty, uint32_t color)
{
    vertex_t a = v;

    a.x += gpu->off_x;
    a.y += gpu->off_y;

    int xmin = max(a.x, gpu->draw_x1);
    int ymin = max(a.y, gpu->draw_y1);
    int xmax = min(xmin + w, gpu->draw_x2);
    int ymax = min(ymin + h, gpu->draw_y2);

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

            gpu->vram[x + (y * 1024)] = texel;
        }

        xc = 0;

        ++yc;
    }
}

PSX_GPU_HOT void gpu_render_flat_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, uint32_t color)
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
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_y1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2);
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_y2);

    for (int y = ymin; y < ymax; y++)
    {
        for (int x = xmin; x < xmax; x++)
        {
            int z0 = ((b.x - a.x) * (y - a.y)) - ((b.y - a.y) * (x - a.x));
            int z1 = ((c.x - b.x) * (y - b.y)) - ((c.y - b.y) * (x - b.x));
            int z2 = ((a.x - c.x) * (y - c.y)) - ((a.y - c.y) * (x - c.x));

            if ((z0 >= 0) && (z1 >= 0) && (z2 >= 0))
            {
                gpu->vram[x + (y * 1024)] = rgb;
            }
        }
    }
}

PSX_GPU_HOT void gpu_render_shaded_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2)
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
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_y1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2);
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_y2);

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

                gpu->vram[x + (y * 1024)] = rgb888_to_rgb565(color);
            }
        }
    }
}

PSX_GPU_HOT void gpu_render_textured_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, uint32_t tpx, uint32_t tpy, uint16_t clutx, uint16_t cluty, int depth)
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
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_y1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2);
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_y2);

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

                gpu->vram[x + (y * 1024)] = rgb888_to_rgb565(color);
            }
        }
    }
}

#define I32(v, b) (((int32_t)((v) << (31 - b))) >> (31 - b))

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

            gpu_render_flat_line(gpu, v0, v1, rgb888_to_rgb565(gpu->buf[0] & 0xffffff));

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
            gpu->xcnt = 0;
            gpu->ycnt = 0;
        }
    }
    break;

    case GPU_STATE_RECV_DATA:
    {
        unsigned int xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
        unsigned int ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;

        gpu->vram[xpos + (ypos * 1024)] = gpu->recv_data & 0xffff;

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

        gpu->vram[xpos + (ypos * 1024)] = gpu->recv_data >> 16;

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

// Monochrome Opaque Quadrilateral
PSX_GPU_HOT void gpu_cmd_28(psx_gpu_t *gpu)
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

            gpu_render_flat_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, rgb888_to_rgb565(gpu->color));
            gpu_render_flat_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, rgb888_to_rgb565(gpu->color));

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

// Monochrome Opaque Quadrilateral
PSX_GPU_HOT void gpu_cmd_30(psx_gpu_t *gpu)
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
PSX_GPU_HOT void gpu_cmd_38(psx_gpu_t *gpu)
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
PSX_GPU_HOT void gpu_cmd_3c(psx_gpu_t *gpu)
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
PSX_GPU_HOT void gpu_cmd_2c(psx_gpu_t *gpu)
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
PSX_GPU_HOT void gpu_cmd_24(psx_gpu_t *gpu)
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
PSX_GPU_HOT void gpu_cmd_2d(psx_gpu_t *gpu)
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

PSX_GPU_HOT void gpu_cmd_64(psx_gpu_t *gpu)
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

PSX_GPU_HOT void gpu_cmd_7c(psx_gpu_t *gpu)
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

PSX_GPU_HOT void gpu_cmd_74(psx_gpu_t *gpu)
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

            gpu_render_textured_rectangle(gpu, gpu->v0, w, h, clutx, cluty, rgb888_to_rgb565(gpu->color));

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_cmd_60(psx_gpu_t *gpu)
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

            gpu_render_flat_rectangle(gpu, gpu->v0, gpu->xsiz, gpu->ysiz, rgb888_to_rgb565(gpu->color));

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_cmd_68(psx_gpu_t *gpu)
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

            gpu->vram[gpu->v0.x + (gpu->v0.y * 1024)] = rgb888_to_rgb565(gpu->color);

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void gpu_cmd_40(psx_gpu_t *gpu)
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

            gpu_render_flat_line(gpu, gpu->v0, gpu->v1, rgb888_to_rgb565(gpu->color));

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

            uint16_t color = rgb888_to_rgb565(gpu->color);

            for (int y = gpu->v0.y; y < (gpu->v0.y + gpu->ysiz); y++)
            {
                for (int x = gpu->v0.x; x < (gpu->v0.x + gpu->xsiz); x++)
                {
                    if ((x < 1024) && (y < 512) && (x >= 0) && (y >= 0))
                        gpu->vram[x + (y * 1024)] = color;
                }
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

            uint32_t srcx = gpu->buf[1] & 0xffff;
            uint32_t srcy = gpu->buf[1] >> 16;
            uint32_t dstx = gpu->buf[2] & 0xffff;
            uint32_t dsty = gpu->buf[2] >> 16;
            uint32_t xsiz = gpu->buf[3] & 0xffff;
            uint32_t ysiz = gpu->buf[3] >> 16;

            for (int y = 0; y < ysiz; y++)
            {
                for (int x = 0; x < xsiz; x++)
                {
                    int dstb = ((dstx + x) < 1024) && ((dsty + y) < 512);
                    int srcb = ((srcx + x) < 1024) && ((srcy + y) < 512);

                    if (dstb && srcb)
                        gpu->vram[(dstx + x) + (dsty + y) * 1024] = gpu->vram[(srcx + x) + (srcy + y) * 1024];
                }
            }

            gpu->state = GPU_STATE_RECV_CMD;
        }
    }
    break;
    }
}

PSX_GPU_HOT void psx_gpu_update_cmd(psx_gpu_t *gpu)
{
    PROF_T0(t_gp0);
    PROF_INC(gp0cmds);
    psx_gpu_update_cmd_impl(gpu);
    PROF_ADD(gp0, t_gp0);
}

static PSX_GPU_HOT void psx_gpu_update_cmd_impl(psx_gpu_t *gpu)
{
    int type = (gpu->buf[0] >> 29) & 7;

    switch (type)
    {
    case 1:
        gpu_poly(gpu);
        return;
    case 2:
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
        gpu_cmd_02(gpu);
        break;
    case 0x24:
        gpu_cmd_24(gpu);
        break;
    case 0x25:
        gpu_cmd_24(gpu);
        break;
    case 0x26:
        gpu_cmd_24(gpu);
        break;
    case 0x27:
        gpu_cmd_24(gpu);
        break;
    case 0x28:
        gpu_cmd_28(gpu);
        break;
    case 0x2a:
        gpu_cmd_28(gpu);
        break;
    case 0x2c:
        gpu_cmd_2d(gpu);
        break;
    case 0x2d:
        gpu_cmd_2d(gpu);
        break;
    case 0x2e:
        gpu_cmd_2d(gpu);
        break;
    case 0x2f:
        gpu_cmd_2d(gpu);
        break;
    case 0x30:
        gpu_cmd_30(gpu);
        break;
    case 0x32:
        gpu_cmd_30(gpu);
        break;
    case 0x38:
        gpu_cmd_38(gpu);
        break;
    case 0x3c:
        gpu_cmd_3c(gpu);
        break;
    case 0x3e:
        gpu_cmd_3c(gpu);
        break;
    case 0x40:
        gpu_cmd_40(gpu);
        break;
    case 0x60:
        gpu_cmd_60(gpu);
        break;
    case 0x62:
        gpu_cmd_60(gpu);
        break;
    case 0x64:
        gpu_cmd_64(gpu);
        break;
    case 0x65:
        gpu_cmd_64(gpu);
        break;
    case 0x66:
        gpu_cmd_64(gpu);
        break;
    case 0x67:
        gpu_cmd_64(gpu);
        break;
    case 0x68:
        gpu_cmd_68(gpu);
        break;
    case 0x74:
        gpu_cmd_74(gpu);
        break;
    case 0x75:
        gpu_cmd_74(gpu);
        break;
    case 0x76:
        gpu_cmd_74(gpu);
        break;
    case 0x77:
        gpu_cmd_74(gpu);
        break;
    case 0x7c:
        gpu_cmd_7c(gpu);
        break;
    case 0x7d:
        gpu_cmd_7c(gpu);
        break;
    case 0x7e:
        gpu_cmd_7c(gpu);
        break;
    case 0x7f:
        gpu_cmd_7c(gpu);
        break;
    case 0x80:
        gpu_cmd_80(gpu);
        break;
    case 0xa0:
        gpu_cmd_a0(gpu);
        break;
    case 0xc0:
        gpu_cmd_c0(gpu);
        break;
    case 0xe1:
    {
        gpu->gpustat &= 0xfffff800;
        gpu->gpustat |= gpu->buf[0] & 0x7ff;
        gpu->texp_x = (gpu->gpustat & 0xf) << 6;
        gpu->texp_y = (gpu->gpustat & 0x10) << 4;
        gpu->texp_d = (gpu->gpustat >> 7) & 0x3;
    }
    break;
    case 0xe2:
    {
        gpu->texw_mx = ((gpu->buf[0] >> 0) & 0x1f) << 3;
        gpu->texw_my = ((gpu->buf[0] >> 5) & 0x1f) << 3;
        gpu->texw_ox = ((gpu->buf[0] >> 10) & 0x1f) << 3;
        gpu->texw_oy = ((gpu->buf[0] >> 15) & 0x1f) << 3;
    }
    break;
    case 0xe3:
    {
        gpu->draw_x1 = (gpu->buf[0] >> 0) & 0x3ff;
        gpu->draw_y1 = (gpu->buf[0] >> 10) & 0x1ff;
    }
    break;
    case 0xe4:
    {
        gpu->draw_x2 = (gpu->buf[0] >> 0) & 0x3ff;
        gpu->draw_y2 = (gpu->buf[0] >> 10) & 0x1ff;
    }
    break;
    case 0xe5:
    {
        gpu->off_x = ((int32_t)(((gpu->buf[0] >> 0) & 0x7ff) << 21)) >> 21;
        gpu->off_y = ((int32_t)(((gpu->buf[0] >> 11) & 0x7ff) << 21)) >> 21;
    }
    break;
    case 0xe6:
    {
        /* To-do: Implement mask bit thing */
    }
    break;
    default:
    {
    }
    break;
    }
}

PSX_GPU_HOT void psx_gpu_write32(psx_gpu_t *gpu, uint32_t offset, uint32_t value)
{
    gpu->vram_dirty = 1;

    switch (offset)
    {
    // GP0
    case 0x00:
    {
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

        switch (cmd)
        {
        // Display enable
        case 0x03:
        {
            gpu->gpustat &= ~0x00800000;
            gpu->gpustat |= (value << 23) & 0x00800000;
        }
        break;
        case 0x04:
        {
        }
        break;
        case 0x05:
        {
            gpu->disp_x = value & 0x3ff;
            gpu->disp_y = (value >> 10) & 0x1ff;
        }
        break;
        case 0x06:
        {
            gpu->disp_x1 = value & 0xfff;
            gpu->disp_x2 = (value >> 12) & 0xfff;
        }
        break;
        case 0x07:
        {
            gpu->disp_y1 = value & 0x1ff;
            gpu->disp_y2 = (value >> 10) & 0x1ff;
        }
        break;
        case 0x08:
            gpu->display_mode = value & 0xffffff;

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



PSX_GPU_HOT void gpu_hblank_event(psx_gpu_t *gpu)
{
    if (gpu->line < GPU_SCANS_PER_VDRAW_NTSC)
    {
        if (gpu->line & 1)
        {
            gpu->gpustat |= 1 << 31;
        }
        else
        {
            gpu->gpustat &= ~(1 << 31);
        }
    }
    else
    {
        gpu->gpustat &= ~(1 << 31);
    }

    gpu->line++;

    if (gpu->line == GPU_SCANS_PER_VDRAW_NTSC)
    {
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

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_gpu_update(psx_gpu_t *gpu, int cyc)
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

void *psx_gpu_get_display_buffer(psx_gpu_t *gpu)
{
    if (gpu->gpustat & 0x800000)
        return gpu->empty;

    return gpu->vram + (gpu->disp_x + (gpu->disp_y * 1024));
}

void psx_gpu_destroy(psx_gpu_t *gpu)
{
    free(gpu->vram);
    free(gpu->empty); // Missing free for the empty buffer!
    free(gpu);
}
