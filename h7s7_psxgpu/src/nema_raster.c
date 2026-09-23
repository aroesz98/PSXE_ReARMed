/*
 * The PSX GPU's primitives on the NeoChrom (GPU2D).
 *
 * gpu.c keeps the command stream, the state and the VRAM; every primitive it
 * would rasterize itself comes here instead (PSX_GPU_EXTERNAL_RASTER) and is
 * turned into NeoChrom commands. Measured on this board, a 24 pixel textured
 * triangle costs 1.4 us of CPU and 0.14 us of GPU here, against 12.5 us for the
 * software rasterizer, and a Gouraud one 2 us against 30 us.
 *
 * VRAM is kept in RGBA5551, not in the PSX's own BGR555: the GPU2D writes no
 * other 16 bit format (a probe at boot prints what it accepts). The conversion
 * is at the edges only - what the game uploads (GP0 A0h) and reads back (C0h) -
 * and it makes the presenter free as well, since the panel can then be fed from
 * VRAM without the CPU repacking anything. A pixel's alpha bit is 1 unless the
 * whole pixel is zero, which is exactly the PSX's "texel 0 is transparent".
 *
 * What is approximated, and why:
 *   - semi transparency is per primitive, not per texel: the PSX picks it from
 *     each texel's top bit, which no blender can do. Mode 2 (B - F) has no
 *     subtracting blend either and is drawn as B * (1 - F), which darkens the
 *     same way; the other three modes are exact.
 *   - a Gouraud shaded *textured* triangle modulates by the average of its
 *     three colours rather than interpolating them, the gradient unit being for
 *     fills; small triangles, so it hardly shows.
 *   - the texture window (GP0 E2h) is ignored - the sampler has no such mask.
 *   - interlaced games that draw one field per frame are drawn whole: skipping
 *     rows is not something the GPU does, and at 0.14 us a triangle the second
 *     field is cheaper than the trouble. The picture comes out progressive.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/logging/log.h>

#include <nema_core.h>
#include <nema_blender.h>
#include <nema_graphics.h>
#include <nema_interpolators.h>

#include "gpu.h"
#include "psx_gpu_platform.h"
#include "nema_port.h"
#include "nema_raster.h"
#include "present.h"

LOG_MODULE_REGISTER(raster, LOG_LEVEL_INF);

/*
    One command list for the whole board, the presenter's (present_cl): drawing
    and showing then go to the GPU in the order they were made, and there is no
    second list to bind, rewind or submit by mistake.
*/
static nema_cmdlist_t *cl;
static bool ready;
static uint32_t queued;          /* primitives waiting for the GPU */
static bool stale;               /* the CPU wrote VRAM: the GPU's read cache is out of date */
/* gpu.c rasterizes itself: the comparison in psx_raster_tritest, and a switch for a debugger */
volatile uint32_t g_raster_bypass;
#define bypass g_raster_bypass

/*
    The palettes the GPU reads.

    Binding a palette puts its address into the command list, so every primitive
    still waiting there is reading it: one buffer, rewritten for the next
    palette, would give all of them the last one - which is what turned the
    picture into coloured speckle. There is a ring of them instead, and when it
    comes round the list is drained first, so that no palette is rewritten while
    anything still refers to it.
*/
#define PAL_SLOTS 32

static uint32_t pal_ring[PAL_SLOTS][256] __nocache __aligned(32);
static uint32_t pal_slot;
static uint32_t *pal = pal_ring[0];
static uint32_t pal_clut = 0xffffffffu;
static uint32_t pal_depth = 0xffffffffu;

/* what the GPU was last told, so that it is not told again */
static uint64_t last_clip = UINT64_MAX;
static uint32_t last_blend = 0xffffffffu;
static uint32_t last_page = 0xffffffffu;  /* texture page: x, y and depth */
static uint32_t last_clut = 0xffffffffu;  /* ... and which palette goes with it */
static uint32_t last_window = 0xffffffffu; /* ... and the texture window it is narrowed to */
static uintptr_t last_dst;

struct psx_raster_stats g_raster;

/* ---- state ------------------------------------------------------------------ */

static void set_dst(psx_gpu_t *gpu)
{
	const uintptr_t dst = (uintptr_t)gpu->vram;

	if (dst != last_dst) {
		last_dst = dst;
		nema_bind_dst_tex(dst, PSX_GPU_FB_WIDTH, PSX_GPU_FB_HEIGHT, NEMA_RGBA5551,
				  PSX_GPU_FB_STRIDE);
		last_clip = UINT64_MAX;
	}
}

static void set_clip(psx_gpu_t *gpu)
{
	/* all four edges: 10 + 9 + 10 bits and the bottom folded into the rest would
	   collide, so the key is 64 bits */
	const uint64_t key = (uint64_t)gpu->draw_x1 | ((uint64_t)gpu->draw_y1 << 16) |
			     ((uint64_t)gpu->draw_x2 << 32) | ((uint64_t)gpu->draw_y2 << 48);

	if (key == last_clip) {
		return;
	}
	last_clip = key;
	nema_set_clip((int)gpu->draw_x1, (int)gpu->draw_y1,
		      (int)gpu->draw_x2 - (int)gpu->draw_x1 + 1,
		      (int)gpu->draw_y2 - (int)gpu->draw_y1 + 1);
}

/*
    The four PSX semi transparency modes, as near as the blender gets.

    The blender has one constant colour, and two things want it: the alpha of
    modes 0 and 3, and the colour a texel is modulated by (NEMA_BLOP_MODULATE_RGB
    multiplies by the constant colour - not by the "texture colour", which is
    for A1/A8 fonts). So this returns the alpha it needs in *alpha and the
    caller sets the constant colour once, with both.

    A textured primitive must still skip its transparent texels (alpha 0, see
    bind_palette) in every mode: the factors are built on the source alpha,
    scaled by the constant alpha where the mode wants a fraction of F.
*/
static uint32_t blend_of(psx_gpu_t *gpu, int transp, int textured, uint32_t *alpha)
{
	*alpha = 255u;

	if (!transp) {
		/* a texel of zero is not drawn; a flat colour always is */
		return textured ? NEMA_BL_SIMPLE : NEMA_BL_SRC;
	}

	switch ((gpu->gpustat >> 5) & 3u) {
	case 0: /* B/2 + F/2 */
		*alpha = 128u;
		return textured ? nema_blending_mode(NEMA_BF_SRCALPHA, NEMA_BF_INVSRCALPHA, NEMA_BLOP_MODULATE_A)
				: nema_blending_mode(NEMA_BF_CONSTALPHA, NEMA_BF_CONSTALPHA, NEMA_BLOP_NONE);
	case 1: /* B + F */
		return textured ? nema_blending_mode(NEMA_BF_SRCALPHA, NEMA_BF_ONE, NEMA_BLOP_NONE) : NEMA_BL_ADD;
	case 2: /* B - F, drawn as B * (1 - F); a transparent texel is black, so it leaves B */
		return nema_blending_mode(NEMA_BF_ZERO, NEMA_BF_INVSRCCOLOR, NEMA_BLOP_NONE);
	default: /* B + F/4 */
		*alpha = 64u;
		return textured ? nema_blending_mode(NEMA_BF_SRCALPHA, NEMA_BF_ONE, NEMA_BLOP_MODULATE_A)
				: nema_blending_mode(NEMA_BF_CONSTALPHA, NEMA_BF_ONE, NEMA_BLOP_NONE);
	}
}

static void set_blend_fill(uint32_t blend)
{
	if (blend != last_blend) {
		last_blend = blend;
		nema_set_blend_fill(blend);
	}
}

static void set_blend_blit(uint32_t blend)
{
	if (blend != last_blend) {
		last_blend = blend;
		nema_set_blend_blit(blend);
	}
}

/* the colour the PSX modulates a texel by: 0x80 means "as it is", and the
   blender cannot brighten, so 0x80 maps to full scale */
static uint32_t modulate_of(uint32_t c)
{
	uint32_t r = (c >> 0) & 0xffu;
	uint32_t g = (c >> 8) & 0xffu;
	uint32_t b = (c >> 16) & 0xffu;

	r = (r > 127u) ? 255u : (r << 1);
	g = (g > 127u) ? 255u : (g << 1);
	b = (b > 127u) ? 255u : (b << 1);

	return nema_rgba((unsigned char)r, (unsigned char)g, (unsigned char)b, 255);
}

static uint32_t color_of(uint32_t c)
{
	return nema_rgba((unsigned char)(c & 0xffu), (unsigned char)((c >> 8) & 0xffu),
			 (unsigned char)((c >> 16) & 0xffu), 255);
}

/* a fill's blend, with the constant alpha it needs */
static uint32_t fill_blend(psx_gpu_t *gpu, int transp)
{
	uint32_t a;
	const uint32_t blend = blend_of(gpu, transp, 0, &a);

	if (a != 255u) {
		nema_set_const_color(nema_rgba(0, 0, 0, (unsigned char)a));
	}
	return blend;
}

/* a blit's blend: the texture page (blop from bind_texture), the transparency
   and the modulation colour (PSX 24 bit, or ~0 for none) */
static void set_blit(psx_gpu_t *gpu, int transp, uint32_t blop, uint32_t mod)
{
	uint32_t a;
	uint32_t blend = blend_of(gpu, transp, 1, &a) | blop;
	uint32_t c = nema_rgba(255, 255, 255, (unsigned char)a);

	if ((mod != 0xffffffffu) && (mod != 0x808080u)) {
		c = (modulate_of(mod) & 0x00ffffffu) | ((a & 0xffu) << 24);
		blend |= NEMA_BLOP_MODULATE_RGB;
	}
	if ((blend & (NEMA_BLOP_MODULATE_RGB | NEMA_BLOP_MODULATE_A)) != 0u) {
		nema_set_const_color(c);
	}
	set_blend_blit(blend);
	last_blend = 0xffffffffu; /* the constant colour rides with the blend */
}

/* the palette of a paletted texture, as the GPU wants it */
static void bind_palette(psx_gpu_t *gpu, uint32_t clut, uint32_t depth)
{
	if ((clut == pal_clut) && (depth == pal_depth)) {
		return;
	}
	pal_clut = clut;
	pal_depth = depth;

	/* the next slot of the ring; when it wraps, what is queued is drawn first */
	if (++pal_slot >= PAL_SLOTS) {
		psx_raster_sync();
		pal_slot = 0;
	}
	pal = pal_ring[pal_slot];

	const uint32_t n = depth ? 256u : 16u;
	const uint32_t x = (clut & 0x3fu) * 16u;
	const uint32_t y = (clut >> 6) & 0x1ffu;
	const uint16_t *src = gpu->vram + (size_t)y * PSX_GPU_FB_WIDTH + x;

	/* the GPU may have drawn into the palette's rows a moment ago */
	sys_cache_data_invd_range((void *)src, n * 2u);

	for (uint32_t i = 0; i < n; i++) {
		const uint32_t v = src[i];
		const uint32_t r = v & 0x1fu;          /* the game uploaded it: BGR555 */
		const uint32_t g = (v >> 5) & 0x1fu;
		const uint32_t b = (v >> 10) & 0x1fu;

		/*
		    An 8 bit index reaches the palette with its nibbles swapped on this
		    GPU2D - 01h picks entry 10h - so every entry is written where the
		    hardware will look for it. The same quirk the NeoChrom showcase
		    found (lut8_slot() in h7s7_neochrom/src/gfx.c). 4 bit indices are
		    not affected.
		*/
		const uint32_t slot = depth ? (((i & 0x0fu) << 4) | ((i >> 4) & 0x0fu)) : i;

		/* a palette entry of zero is the transparent one */
		pal[slot] = v ? (0xff000000u | (((r << 3) | (r >> 2)) << 16) |
				 (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2)))
			      : 0u;
	}
	g_raster.palettes++;
}

/*
    The texture page as a source, narrowed to the texture window.

    GP0(E2h) masks the texture coordinates: the texel is
    (u & ~mask_x) | (offset_x & mask_x), which repeats a rectangle of
    (~mask_x & 0xff) + 1 texels - always a power of two - from that offset. The
    sampler has no such mask, but it wraps: binding exactly that rectangle and
    letting the coordinates repeat gives the same texel for every u. With no
    window (mask 0) the rectangle is the whole 256x256 page, so one path does
    both. Without this, in-game textures come out as structured noise while
    menus, which set no window, look right.
*/
static uint32_t bind_texture(psx_gpu_t *gpu, uint32_t clut)
{
	const uint32_t t0 = k_cycle_get_32();
	const uint32_t depth = gpu->texp_d;
	const uint32_t tw = ((~gpu->texw_mx) & 0xffu) + 1u;
	const uint32_t th = ((~gpu->texw_my) & 0xffu) + 1u;
	const uint32_t ox = gpu->texw_ox & gpu->texw_mx;
	const uint32_t oy = gpu->texw_oy & gpu->texw_my;
	const uint32_t page = (uint32_t)gpu->texp_x | ((uint32_t)gpu->texp_y << 10) | (depth << 20);
	const uint32_t window = ox | (oy << 8) | (tw << 16) | (th << 24);

	/* the offset into the page, in halfwords: four texels to one at 4 bits, two at 8 */
	const uint32_t xoff = depth ? (depth >= 2u ? ox : (ox >> 1)) : (ox >> 2);
	const uintptr_t base =
		(uintptr_t)(gpu->vram + (size_t)(gpu->texp_y + oy) * PSX_GPU_FB_WIDTH + gpu->texp_x + xoff);

	if ((page == last_page) && (window == last_window) && ((depth >= 2u) || (clut == last_clut))) {
		g_raster.bind_cyc += k_cycle_get_32() - t0;

		return (depth < 2u) ? NEMA_BLOP_LUT : 0u;
	}
	last_page = page;
	last_window = window;
	last_clut = clut;
	g_raster.tex_binds++;

	if (depth >= 2u) {
		/* 15 bit texels: uploaded as BGR555, read as RGBA5551, so their red and
		   blue come out swapped - counted, to show how much a game uses them */
		g_raster.tex15++;
		nema_bind_src_tex(base, tw, th, NEMA_RGBA5551, PSX_GPU_FB_STRIDE,
				  NEMA_FILTER_PS | NEMA_TEX_REPEAT);
		g_raster.bind_cyc += k_cycle_get_32() - t0;

		return 0u;
	}

	bind_palette(gpu, clut, depth);
	nema_bind_lut_tex(base, tw, th, depth ? NEMA_L8 : NEMA_L4LE, PSX_GPU_FB_STRIDE,
			  NEMA_FILTER_PS | NEMA_TEX_REPEAT, (uintptr_t)pal, NEMA_BGRA8888);
	g_raster.bind_cyc += k_cycle_get_32() - t0;

	return NEMA_BLOP_LUT;
}

/* ---- the command list ---------------------------------------------------------- */

void psx_raster_init(void)
{
	cl = (nema_cmdlist_t *)present_cl();
	if ((cl == NULL) || (cl->bo.base_virt == NULL)) {
		LOG_ERR("no command list");
		return;
	}
	ready = true;

	/*
	    Commands go into whichever list is bound, and the presenter binds its
	    own: every function here that writes commands binds this one first, and
	    the presenter binds its list back when it draws. Without that, the
	    primitives ended up in the presenter's circular list and this one was
	    submitted empty - which is what a texture self test of all zeroes said.
	*/
	LOG_INF("primitives go to the NeoChrom, through the presenter's command list");
}

/*
    Everything queued is drawn, and the CPU may touch VRAM again.

    Called before every upload, copy and read back. Whatever the CPU does to
    VRAM afterwards the GPU must not serve from its own read cache, which is not
    coherent, so the cache is marked out of date here and invalidated in front
    of the next primitive - the invalidation is a command in the list, so it has
    to go in once there is a list to put it in.
*/
void psx_raster_sync(void)
{
	if (!ready) {
		return;
	}

	/*
	    The CPU is about to change VRAM, and what it changes may be the very
	    texture or palette the GPU was last given: a game reloads a palette to
	    the same address all the time. So everything remembered about the
	    binding goes, and the next primitive says it all again.
	*/
	stale = true;
	last_page = 0xffffffffu;
	last_clut = 0xffffffffu;
	last_window = 0xffffffffu;
	pal_clut = 0xffffffffu;
	pal_depth = 0xffffffffu;

	if (!queued) {
		return;
	}

	const uint32_t t0 = k_cycle_get_32();

	nema_cl_submit(cl);
	(void)nema_cl_wait(cl);

	if (nema_port_hung()) {
		LOG_ERR("GPU2D hang while drawing");
		nema_port_reset_gpu();
		(void)nema_reinit();
		nema_ext_hold_irq_enable(2);
		nema_ext_hold_irq_enable(3);
		g_raster.hangs++;
	}
	g_raster.gpu_cyc += k_cycle_get_32() - t0;
	g_raster.flushes++;
	queued = 0;

	/* the next primitive says everything again: the list starts empty */
	last_dst = 0;
	last_clip = UINT64_MAX;
	last_blend = 0xffffffffu;
	last_page = 0xffffffffu;
	last_clut = 0xffffffffu;
	last_window = 0xffffffffu;
}

/* room for one more primitive, or the list goes out */
static bool room(psx_gpu_t *gpu)
{
	ARG_UNUSED(gpu);

	if (!ready || bypass) {
		return false;
	}
	if (nema_cl_almost_full(cl)) {
		psx_raster_sync();
	}
	if (stale) {
		/* the CPU has written VRAM since the GPU last read it */
		stale = false;
		platform_invalidate_cache();
		g_raster.invalidations++;
	}
	queued++;
	g_raster.prims++;
	return true;
}

/* ---- the primitives -------------------------------------------------------------- */

/*
    A textured triangle, the way the PSX maps it: the texture coordinates are
    an affine function of the screen position, fixed by the three vertices.

    nema_blit_tri_uv() draws nothing when the texture triangle has no area -
    which games do all the time: a sky or a floor fade is a quad whose texels
    all sit in one column (Tekken 3's stages), a line of texels stretched. Such
    a triangle is given a sliver of an area, 0.4 texel at two corners: every
    texture coordinate moves by less than half a texel and only upwards, so a
    texel that is sampled exactly (the column of a fade) stays the same.
    The texture coordinates are all whole numbers, so "no area" is exactly 0.

    (Solving the screen -> texture matrix here and drawing with
    nema_blit_quad_m(), the fourth corner repeating the third, maps the same -
    but the degenerate quad leaves stray spans and single pixels all over the
    picture, far from the triangle: measured against gpu.c on a Tekken 3 capture.)

    Where the pixels are sampled - the corner against the centre - is settled
    by the caller moving the triangle (psx_raster_triangle).
*/
static void blit_tri_affine(float x0, float y0, float x1, float y1, float x2, float y2, vertex_t t0,
			    vertex_t t1, vertex_t t2)
{
	const float dx1 = x1 - x0, dy1 = y1 - y0;
	const float dx2 = x2 - x0, dy2 = y2 - y0;
	const float det = dx1 * dy2 - dx2 * dy1;

	if (det == 0.f) {
		return; /* no area on screen: the PSX draws nothing either */
	}
	float su0 = (float)t0.tx, sv0 = (float)t0.ty;
	float su1 = (float)t1.tx, sv1 = (float)t1.ty;
	float su2 = (float)t2.tx, sv2 = (float)t2.ty;

	if ((su1 - su0) * (sv2 - sv0) == (su2 - su0) * (sv1 - sv0)) {
		su1 += 0.4f;
		sv2 += 0.4f;
	}
	nema_blit_tri_uv(x0, y0, 1.f, x1, y1, 1.f, x2, y2, 1.f, su0, sv0, su1, sv1, su2, sv2);
}

int psx_raster_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data)
{
	if (!room(gpu)) {
		return 0;
	}

	const uint32_t t_cpu = k_cycle_get_32();

	const int textured = (data.attrib & PA_TEXTURED) != 0;
	const int shaded = (data.attrib & PA_SHADED) != 0;
	const int raw = (data.attrib & PA_RAW) != 0;
	const int transp = (data.attrib & PA_TRANSP) != 0;

	/*
	    The PSX draws a pixel when its top left corner is inside the triangle -
	    the left and top edges in, the right and bottom ones out - and the GPU2D
	    when its centre is. Moved by (almost) half a pixel, the one test is the
	    other: otherwise the GPU2D draws one more column and row at the right
	    and bottom edges, with texels from just outside the polygon's part of
	    the texture page - a line of another picture's colours along the seams
	    of every tiled background. The texture moves with the triangle, so it
	    is sampled where the PSX samples it (and 1/64 of a pixel on, which keeps
	    a whole texel from rounding down to the one before it).
	*/
	const float c = 0.5f - (1.f / 64.f);
	const float x0 = (float)(v0.x + gpu->off_x) + c, y0 = (float)(v0.y + gpu->off_y) + c;
	const float x1 = (float)(v1.x + gpu->off_x) + c, y1 = (float)(v1.y + gpu->off_y) + c;
	const float x2 = (float)(v2.x + gpu->off_x) + c, y2 = (float)(v2.y + gpu->off_y) + c;

	set_dst(gpu);
	set_clip(gpu);

	if (!textured) {
		const uint32_t blend = fill_blend(gpu, transp);

		if (shaded) {
			color_var_t c0 = {(float)((v0.c >> 0) & 0xffu), (float)((v0.c >> 8) & 0xffu),
					  (float)((v0.c >> 16) & 0xffu), 255.f};
			color_var_t c1 = {(float)((v1.c >> 0) & 0xffu), (float)((v1.c >> 8) & 0xffu),
					  (float)((v1.c >> 16) & 0xffu), 255.f};
			color_var_t c2 = {(float)((v2.c >> 0) & 0xffu), (float)((v2.c >> 8) & 0xffu),
					  (float)((v2.c >> 16) & 0xffu), 255.f};

			nema_enable_gradient(1);
			set_blend_fill(blend);
			nema_interpolate_tri_colors(x0, y0, x1, y1, x2, y2, &c0, &c1, &c2);
			nema_fill_triangle_f(x0, y0, x1, y1, x2, y2, 0xffffffffu);
			nema_enable_gradient(0);
			last_blend = 0xffffffffu; /* the gradient switch changes the blender */
			g_raster.gouraud++;
		} else {
			set_blend_fill(blend);
			nema_fill_triangle_f(x0, y0, x1, y1, x2, y2, color_of(v0.c));
			g_raster.flat++;
		}
		g_raster.cpu_cyc += k_cycle_get_32() - t_cpu;

		return 1;
	}

	/* textured: the page and its palette, then the triangle with its texture coordinates */
	const uint32_t blop = bind_texture(gpu, data.clut);
	uint32_t mod = 0xffffffffu;

	if (!raw) {
		/* the texel is modulated by the vertex colour; a Gouraud one is
		   averaged, the gradient unit being for fills */
		uint32_t c = v0.c;

		if (shaded) {
			const uint32_t r = (((v0.c >> 0) & 0xffu) + ((v1.c >> 0) & 0xffu) + ((v2.c >> 0) & 0xffu)) / 3u;
			const uint32_t g = (((v0.c >> 8) & 0xffu) + ((v1.c >> 8) & 0xffu) + ((v2.c >> 8) & 0xffu)) / 3u;
			const uint32_t b = (((v0.c >> 16) & 0xffu) + ((v1.c >> 16) & 0xffu) + ((v2.c >> 16) & 0xffu)) / 3u;

			c = r | (g << 8) | (b << 16);
		}
		mod = c;
	}
	set_blit(gpu, transp, blop, mod);

	blit_tri_affine(x0, y0, x1, y1, x2, y2, v0, v1, v2);
	g_raster.textured++;
	g_raster.cpu_cyc += k_cycle_get_32() - t_cpu;

	return 1;
}

int psx_raster_rect(psx_gpu_t *gpu, rect_data_t data)
{
	if (!room(gpu)) {
		return 0;
	}

	const int textured = (data.attrib & RA_TEXTURED) != 0;
	const int raw = (data.attrib & RA_RAW) != 0;
	const int transp = (data.attrib & RA_TRANSP) != 0;
	const int x = data.v0.x + gpu->off_x;
	const int y = data.v0.y + gpu->off_y;
	const int w = (int)data.width;
	const int h = (int)data.height;

	set_dst(gpu);
	set_clip(gpu);

	if (!textured) {
		set_blend_fill(fill_blend(gpu, transp));
		nema_fill_rect(x, y, w, h, color_of(data.v0.c));
		g_raster.rects++;
		return 1;
	}

	const uint32_t blop = bind_texture(gpu, data.clut);

	set_blit(gpu, transp, blop, raw ? 0xffffffffu : (data.v0.c & 0xffffffu));

	nema_blit_subrect(x, y, w, h, (int)data.v0.tx, (int)data.v0.ty);
	g_raster.rects++;
	return 1;
}

int psx_raster_line(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, uint32_t color, int transp)
{
	if (!room(gpu)) {
		return 0;
	}

	/* the colour comes as a VRAM pixel: rgb888_to_bgr555 in gpu.c converts to
	   RGBA5551 on this board (PSX_PIX_FROM_PSX) */
	const uint32_t r = ((color >> 11) & 0x1fu) << 3;
	const uint32_t g = ((color >> 6) & 0x1fu) << 3;
	const uint32_t b = ((color >> 1) & 0x1fu) << 3;

	set_dst(gpu);
	set_clip(gpu);
	set_blend_fill(fill_blend(gpu, transp));
	nema_draw_line(v0.x + gpu->off_x, v0.y + gpu->off_y, v1.x + gpu->off_x, v1.y + gpu->off_y,
		       nema_rgba((unsigned char)r, (unsigned char)g, (unsigned char)b, 255));
	g_raster.lines++;
	return 1;
}

/* GP0(02h): a rectangle of VRAM set to a colour, outside the drawing area and
   with no blending */
int psx_raster_fill(psx_gpu_t *gpu, int x, int y, int w, int h, uint32_t color)
{
	if (!room(gpu)) {
		return 0;
	}
	set_dst(gpu);
	nema_set_clip(0, 0, PSX_GPU_FB_WIDTH, PSX_GPU_FB_HEIGHT);
	last_clip = UINT64_MAX;
	set_blend_fill(NEMA_BL_SRC);
	nema_fill_rect(x, y, w, h, color_of(color));
	g_raster.rects++;
	return 1;
}

/*
    Does the GPU read a PSX texture the way the PSX does?

    A 16 texel wide 4 bit texture whose index is its own position, a palette
    whose entry i is the colour (i*2, 0, 0), and a 16x1 blit: what comes out
    should be 0, 2, 4 ... 30 in red. Anything else says how the sampler differs -
    nibbles the other way round, a different row stride, a shifted palette. The
    same again for 8 bit indices. The answers are left in g_raster_test for a
    debugger to read, and printed.
*/
uint32_t g_raster_test[40];

void psx_raster_selftest(psx_gpu_t *gpu)
{
	static uint16_t dst[32] __nocache __aligned(32);
	uint16_t *tex = gpu->vram + 16u * PSX_GPU_FB_WIDTH; /* a row of VRAM nothing else uses */

	if (!ready) {
		return;
	}
	/* 4 bit: four texels to a halfword, the lowest nibble first */
	for (uint32_t i = 0; i < 4u; i++) {
		tex[i] = (uint16_t)((i * 4u + 0u) | ((i * 4u + 1u) << 4) | ((i * 4u + 2u) << 8) |
				    ((i * 4u + 3u) << 12));
	}
	/* 8 bit, on the next row: two texels to a halfword */
	for (uint32_t i = 0; i < 8u; i++) {
		tex[PSX_GPU_FB_WIDTH + i] = (uint16_t)((i * 2u) | ((i * 2u + 1u) << 8));
	}
	sys_cache_data_flush_range(tex, 2u * PSX_GPU_FB_STRIDE);

	for (uint32_t depth = 0; depth < 2u; depth++) {
		pal = pal_ring[0];
		for (uint32_t i = 0; i < 256u; i++) {
			pal[i] = 0xff000000u | ((((i ^ (i >> 4)) & 15u) * 16u) << 16); /* red says which entry it is */
		}
		memset(dst, 0, sizeof(dst));
		sys_cache_data_flush_and_invd_range(dst, sizeof(dst));

		nema_bind_dst_tex((uintptr_t)dst, 16, 1, NEMA_RGBA5551, 32);
		nema_set_clip(0, 0, 16, 1);
		platform_invalidate_cache();
		nema_bind_lut_tex((uintptr_t)(tex + depth * PSX_GPU_FB_WIDTH), 16, 1,
				  depth ? NEMA_L8 : NEMA_L4LE, PSX_GPU_FB_STRIDE, NEMA_FILTER_PS,
				  (uintptr_t)pal, NEMA_BGRA8888);
		nema_set_blend_blit(NEMA_BL_SRC | NEMA_BLOP_LUT);
		nema_blit_subrect(0, 0, 16, 1, 0, 0);
		nema_cl_submit(cl);
		(void)nema_cl_wait(cl);

		char line[120];
		size_t n = 0;

		for (uint32_t i = 0; i < 16u; i++) {
			/* the destination is RGBA5551: red is the top five bits */
			const uint32_t got = (dst[i] >> 11) & 0x1fu;

			g_raster_test[depth * 16u + i] = got;
			n += snprintf(line + n, sizeof(line) - n, " %u", got);
		}
		LOG_INF("%u bit texels 0..15 read back as:%s (want 0 2 4 6 ... 30)",
			depth ? 8u : 4u, line);
	}

	/* the state the test left behind is not the state the game expects */
	last_dst = 0;
	last_clip = UINT64_MAX;
	last_blend = 0xffffffffu;
	last_page = 0xffffffffu;
	last_clut = 0xffffffffu;
	last_window = 0xffffffffu;
	pal_clut = 0xffffffffu;
	pal_depth = 0xffffffffu;
	memset(tex, 0, 2u * PSX_GPU_FB_STRIDE);
	sys_cache_data_flush_range(tex, 2u * PSX_GPU_FB_STRIDE);
}

/*
    The primitives a game sends, drawn both ways.

    A handful of GP0 commands - textured triangle, quad, Gouraud textured
    triangle, sprite, flat triangle - go through gpu.c once with the NeoChrom
    drawing them and once with gpu.c's own rasterizer, into the same corner of
    VRAM, and the two pictures are compared shape by shape. The software one is
    the reference (it is what the RT1050 shows correctly). Both pictures stay in
    g_tritest_img for a debugger: [0] the NeoChrom's, [1] the reference, both in
    RGBA5551, TT_W x TT_H from VRAM (0, TT_Y).
*/
#define TT_Y 100u
#define TT_W 384u
#define TT_H 160u

uint16_t g_tritest_img[2][TT_H * TT_W]
	__attribute__((section(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(psram))))) __aligned(64);
uint32_t g_tritest[18]; /* per shape: pixels drawn by the NeoChrom, by the reference, differing */

static void tt_gp0(psx_gpu_t *gpu, const uint32_t *w, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		psx_gpu_write32(gpu, 0, w[i]);
	}
}

static void tt_draw(psx_gpu_t *gpu)
{
	const uint32_t clut = 500u << 6;  /* palette at (0, 500) */
	const uint32_t tpage = 8u | (1u << 4); /* 4 bit texture page at (512, 256) */
#define XY(x, y) ((uint32_t)(x) | ((uint32_t)(y) << 16))
#define UV(u, v, hi) ((uint32_t)(u) | ((uint32_t)(v) << 8) | ((uint32_t)(hi) << 16))
	const uint32_t env[] = {
		0xe1000000u | tpage,
		0xe2000000u,
		0xe3000000u,
		0xe4000000u | 383u | (479u << 10),
		0xe5000000u,
	};
	/* raw textured triangle */
	const uint32_t tri[] = {0x25808080u, XY(10, 105), UV(0, 0, clut), XY(110, 105), UV(100, 0, tpage),
				XY(10, 195), UV(0, 90, 0)};
	/* raw textured quad */
	const uint32_t quad[] = {0x2d808080u, XY(130, 105), UV(0, 0, clut), XY(230, 105), UV(63, 0, tpage),
				 XY(130, 195), UV(0, 63, 0), XY(230, 195), UV(63, 63, 0)};
	/* Gouraud textured triangle, darker at two corners */
	const uint32_t gtri[] = {0x34808080u, XY(250, 105), UV(0, 0, clut), 0x00404040u, XY(370, 105),
				 UV(120, 0, tpage), 0x00800040u, XY(250, 195), UV(0, 90, 0)};
	/* raw sprite */
	const uint32_t sprite[] = {0x65808080u, XY(10, 210), UV(8, 16, clut), XY(64, 40)};
	/* flat triangle */
	const uint32_t flat[] = {0x2000c000u, XY(130, 210), XY(230, 210), XY(130, 255)};
	/* modulated quad whose texels are all in one column (a sky fade), partly off the left edge */
	const uint32_t dquad[] = {0x2c606060u, XY(250, 208), UV(21, 0, clut), XY(370, 208), UV(21, 0, tpage),
				  XY(250, 255), UV(21, 40, 0), XY(370, 255), UV(21, 40, 0)};

	tt_gp0(gpu, env, ARRAY_SIZE(env));
	tt_gp0(gpu, tri, ARRAY_SIZE(tri));
	tt_gp0(gpu, quad, ARRAY_SIZE(quad));
	tt_gp0(gpu, gtri, ARRAY_SIZE(gtri));
	tt_gp0(gpu, sprite, ARRAY_SIZE(sprite));
	tt_gp0(gpu, flat, ARRAY_SIZE(flat));
	tt_gp0(gpu, dquad, ARRAY_SIZE(dquad));
	psx_raster_sync();
#undef XY
#undef UV
}

static void tt_clear(psx_gpu_t *gpu)
{
	psx_raster_sync();
	for (uint32_t y = 0; y < TT_H; y++) {
		memset(gpu->vram + (TT_Y + y) * PSX_GPU_FB_WIDTH, 0, TT_W * 2u);
	}
	sys_cache_data_flush_range(gpu->vram + TT_Y * PSX_GPU_FB_WIDTH, TT_H * PSX_GPU_FB_STRIDE);
}

static void tt_grab(psx_gpu_t *gpu, uint16_t *out, bool convert)
{
	sys_cache_data_invd_range(gpu->vram + TT_Y * PSX_GPU_FB_WIDTH, TT_H * PSX_GPU_FB_STRIDE);
	for (uint32_t y = 0; y < TT_H; y++) {
		const uint16_t *s = gpu->vram + (TT_Y + y) * PSX_GPU_FB_WIDTH;

		for (uint32_t x = 0; x < TT_W; x++) {
			out[y * TT_W + x] = convert ? PSX_PIX_FROM_PSX(s[x]) : s[x];
		}
	}
}

void psx_raster_tritest(psx_gpu_t *gpu)
{
	static const struct {
		const char *name;
		uint16_t x0, y0, x1, y1; /* the shape's box, in VRAM */
	} shape[] = {
		{"tri", 0, 100, 120, 200},  {"quad", 120, 100, 240, 200}, {"gtri", 240, 100, 384, 200},
		{"sprite", 0, 205, 120, 260}, {"flat", 120, 205, 240, 260}, {"dquad", 240, 205, 384, 260},
	};

	if (!ready) {
		return;
	}

	/* the texture: 4 bit, index = (u + (v / 8)) & 15 - a new texel every column, so that
	   two neighbours swapped (the nibble order) shows; the palette: 16 colours, none zero */
	psx_raster_sync();
	for (uint32_t v = 0; v < 256u; v++) {
		uint16_t *row = gpu->vram + (256u + v) * PSX_GPU_FB_WIDTH + 512u;

		for (uint32_t h = 0; h < 64u; h++) {
			uint16_t w = 0;

			for (uint32_t k = 0; k < 4u; k++) {
				w |= (uint16_t)((((h * 4u + k) + v / 8u) & 15u) << (4u * k));
			}
			row[h] = w;
		}
	}
	for (uint32_t i = 0; i < 16u; i++) {
		gpu->vram[500u * PSX_GPU_FB_WIDTH + i] =
			(uint16_t)(((i * 2u + 1u) & 31u) | ((31u - i * 2u) << 5) | (((i * 5u) & 31u) << 10));
	}
	sys_cache_data_flush_range(gpu->vram + 256u * PSX_GPU_FB_WIDTH, 256u * PSX_GPU_FB_STRIDE);
	sys_cache_data_flush_range(gpu->vram + 500u * PSX_GPU_FB_WIDTH, PSX_GPU_FB_STRIDE);

	tt_clear(gpu);
	tt_draw(gpu);
	tt_grab(gpu, g_tritest_img[0], false);

	tt_clear(gpu);
	bypass = true;
	tt_draw(gpu);
	bypass = false;
	tt_grab(gpu, g_tritest_img[1], true);
	tt_clear(gpu);

	for (size_t s = 0; s < ARRAY_SIZE(shape); s++) {
		uint32_t n_gpu = 0, n_ref = 0, n_diff = 0;

		for (uint32_t y = shape[s].y0; y < shape[s].y1; y++) {
			for (uint32_t x = shape[s].x0; x < shape[s].x1; x++) {
				const uint16_t a = g_tritest_img[0][(y - TT_Y) * TT_W + x];
				const uint16_t b = g_tritest_img[1][(y - TT_Y) * TT_W + x];

				n_gpu += (a != 0);
				n_ref += (b != 0);
				/* a colour step either way is rounding, not a difference */
				const int dr = (int)((a >> 11) & 31u) - (int)((b >> 11) & 31u);
				const int dg = (int)((a >> 6) & 31u) - (int)((b >> 6) & 31u);
				const int db = (int)((a >> 1) & 31u) - (int)((b >> 1) & 31u);

				n_diff += ((a != 0) != (b != 0)) || (dr > 1) || (dr < -1) || (dg > 1) || (dg < -1) ||
					  (db > 1) || (db < -1);
			}
		}
		g_tritest[s * 3u + 0u] = n_gpu;
		g_tritest[s * 3u + 1u] = n_ref;
		g_tritest[s * 3u + 2u] = n_diff;
		LOG_INF("tritest %-6s: neochrom %5u px, reference %5u px, %5u differ", shape[s].name, n_gpu, n_ref,
			n_diff);
	}
}

void psxgpu_vram_cpu_read(const uint16_t *vram, uint32_t y, uint32_t rows)
{
	if ((y + rows) > PSX_GPU_FB_HEIGHT) {
		/* wraps round the bottom of VRAM: all of it */
		y = 0;
		rows = PSX_GPU_FB_HEIGHT;
	}
	sys_cache_data_invd_range((void *)(vram + (size_t)y * PSX_GPU_FB_WIDTH), (size_t)rows * PSX_GPU_FB_STRIDE);
}

void psx_raster_get_stats(struct psx_raster_stats *out)
{
	*out = g_raster;
}

void psx_raster_reset_stats(void)
{
	memset(&g_raster, 0, sizeof(g_raster));
}
