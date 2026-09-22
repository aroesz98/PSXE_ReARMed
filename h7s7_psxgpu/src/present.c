/*
 * Presenting the PSX picture on the STM32H7S78-DK panel.
 *
 * The display window of VRAM (15 bit BGR or packed 24 bit) is repacked by the
 * CPU into an RGB565 staging picture - only the rows the GPU drew into since
 * the last present, the same dirty-row tracking the RT1050 uses - and the
 * NeoChrom scales it into a frame buffer: point sampled when the ratio is
 * whole (320x240 -> 640x480), bilinear otherwise, letterboxed to 4:3.
 * The LTDC layer address is a shadow register reloaded at the next vertical
 * blank, so a flip never tears and never waits: three buffers - on screen,
 * queued for the blank, being drawn.
 *
 * Interlaced 480 line games that draw one field per frame (Tekken 3) are
 * shown one field at a time, the field the game just finished, scaled to the
 * panel's 480 lines; the other one would be a frame older and would comb.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/cache.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <soc.h>

#include <nema_core.h>
#include <nema_interpolators.h>
#include <nema_provisional.h>
#include <nema_raster.h>

#include "nema_port.h"
#include "present.h"

LOG_MODULE_REGISTER(present, LOG_LEVEL_INF);

#define FB_W 800
#define FB_H 480
#define NFB 3

#define STAGE_W 640
#define STAGE_H 480

#define PSRAM_SECTION __attribute__((section(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(psram)))))

static uint16_t fb_mem[NFB][FB_W * FB_H] PSRAM_SECTION __aligned(64);
static uint16_t stage[STAGE_W * STAGE_H] PSRAM_SECTION __aligned(64);
static uint8_t cl_mem[8192] __aligned(32);
static nema_cmdlist_t cl;

extern const uint8_t font8x8_a1[768];
static uint8_t font_mem[768] __aligned(32);

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* VRAM read by the GPU as it is: a PSX pixel is 0BBBBBGGGGGRRRRR with the mask bit on
   top, which in NemaGFX's naming is ABGR1555 - "available if HW enabled", so it is
   tried once at boot with a readback, and the CPU repack stays as the fallback */
static uint16_t probe_src[64] __nocache __aligned(32);
static uint16_t probe_dst[64] __nocache __aligned(32);
static bool direct_ok;
static nema_tex_format_t direct_src_fmt; /* how the GPU reads VRAM */
static nema_tex_format_t direct_dst_fmt; /* how it writes the frame buffer then (RGB565 in memory either way) */

static bool probe_abgr1555(void);

enum fb_state { FB_FREE, FB_QUEUED, FB_FRONT, FB_BUSY };
static uint8_t fb_st[NFB];
static struct present_stats st;

/* what the staging picture holds, to repack only what changed */
static uint32_t staged_x = ~0u, staged_y = ~0u;
static int32_t staged_w = -1, staged_h = -1, staged_24 = -1, staged_step = -1;
static int32_t last_scaled_w = -1, last_scaled_h = -1;
static int clear_pending;

/* the picture the GPU is still scaling: it goes to the panel once the GPU is done,
   checked at every vertical blank, so the scaling overlaps the next frame's drawing */
static void flip_queue(int i);

static int pending_fb = -1;
static int32_t pending_id;
static uint32_t pending_since;

static void gpu_recover(void)
{
	LOG_ERR("GPU2D hang while presenting");
	nema_port_reset_gpu();
	(void)nema_reinit();
	nema_ext_hold_irq_enable(2);
	nema_ext_hold_irq_enable(3);
	nema_cl_rewind(&cl);
	nema_cl_bind_circular(&cl);
}

static void pending_poll(void)
{
	if (pending_fb < 0) {
		return;
	}
	if (nema_port_cl_done(pending_id)) {
		st.gpu_us = k_cyc_to_us_floor32(k_cycle_get_32() - pending_since);
		flip_queue(pending_fb);
		pending_fb = -1;
		return;
	}
	if (k_cyc_to_ms_floor32(k_cycle_get_32() - pending_since) > 200) {
		gpu_recover();
		fb_st[pending_fb] = FB_FREE;
		pending_fb = -1;
	}
}

static void pending_wait(void)
{
	if (pending_fb >= 0) {
		(void)nema_port_wait_cl(pending_id);
		pending_poll();
	}
}

/* ---- LTDC flip (see gfx.c of the showcase) -------------------------------- */

static bool flip_pending(void)
{
	return (LTDC->SRCR & LTDC_SRCR_VBR) != 0U;
}

static void flip_poll(void)
{
	if (flip_pending()) {
		return;
	}
	for (int i = 0; i < NFB; i++) {
		if (fb_st[i] == FB_QUEUED) {
			for (int j = 0; j < NFB; j++) {
				if (fb_st[j] == FB_FRONT) {
					fb_st[j] = FB_FREE;
				}
			}
			fb_st[i] = FB_FRONT;
		}
	}
}

static void flip_queue(int i)
{
	LTDC_Layer1->CFBAR = (uint32_t)fb_mem[i];
	LTDC->SRCR = LTDC_SRCR_VBR;
	fb_st[i] = FB_QUEUED;
}

static int pick_free_fb(void)
{
	flip_poll();
	for (int i = 0; i < NFB; i++) {
		if (fb_st[i] == FB_FREE) {
			return i;
		}
	}
	return -1;
}

/* ---- init ----------------------------------------------------------------- */

int present_init(void)
{
	if (!device_is_ready(disp)) {
		LOG_ERR("display not ready");
		return -ENODEV;
	}
	int ret = nema_init();

	if (ret != 0) {
		LOG_ERR("nema_init failed: %d", ret);
		return -EIO;
	}
	nema_ext_hold_irq_enable(2);
	nema_ext_hold_irq_enable(3);

	nema_buffer_t bo = {
		.base_virt = cl_mem,
		.base_phys = (uintptr_t)cl_mem,
		.size = sizeof(cl_mem),
		.fd = 0,
	};

	nema_port_set_cl_buffer(cl_mem, sizeof(cl_mem));
	cl = nema_cl_create_prealloc(&bo);
	if (cl.bo.base_virt == NULL) {
		return -ENOMEM;
	}
	nema_cl_bind_circular(&cl);

	memcpy(font_mem, font8x8_a1, sizeof(font_mem));
	sys_cache_data_flush_range(font_mem, sizeof(font_mem));

	for (int i = 0; i < NFB; i++) {
		memset(fb_mem[i], 0, sizeof(fb_mem[i]));
		sys_cache_data_flush_and_invd_range(fb_mem[i], sizeof(fb_mem[i]));
		fb_st[i] = FB_FREE;
	}
	memset(stage, 0, sizeof(stage));
	sys_cache_data_flush_and_invd_range(stage, sizeof(stage));

	struct display_buffer_descriptor desc = {
		.buf_size = FB_W * FB_H * 2,
		.width = FB_W,
		.height = FB_H,
		.pitch = FB_W,
	};

	(void)display_write(disp, 0, 0, &desc, fb_mem[0]);
	fb_st[0] = FB_FRONT;
	(void)display_blanking_off(disp);
	clear_pending = NFB;
	direct_ok = probe_abgr1555();
	LOG_INF("NeoChrom %s, panel %dx%d", nema_get_sw_device_name(), FB_W, FB_H);
	return 0;
}

/* ---- repack ---------------------------------------------------------------- */

/* the display window as the emulator computes it (psx.c) */
static uint32_t dmode_width(const psx_gpu_t *gpu)
{
	static const uint32_t hres[4] = {256, 320, 512, 640};

	return (gpu->display_mode & 0x40u) ? 368u : hres[gpu->display_mode & 3u];
}

static uint32_t dmode_height(const psx_gpu_t *gpu)
{
	if ((gpu->display_mode & 0x4u) && (gpu->display_mode & 0x20u)) {
		return 480u;
	}
	int32_t d = (int32_t)gpu->disp_y2 - (int32_t)gpu->disp_y1;

	return (d < (255 - 16)) ? (uint32_t)d : 240u;
}

static void repack_bgr555(const uint16_t *src, int32_t w, int32_t rows, int32_t step, uint16_t *dst)
{
	for (int32_t y = 0; y < rows; y++) {
		const uint16_t *s = src + (size_t)y * step * PSX_GPU_FB_WIDTH;
		uint16_t *d = dst + (size_t)y * w;
		int32_t x = 0;

		/* two pixels per word: the three fields move as pairs */
		if (((((uintptr_t)s) | ((uintptr_t)d)) & 3u) == 0u) {
			const uint32_t *s32 = (const uint32_t *)s;
			uint32_t *d32 = (uint32_t *)d;

			for (; x + 3 < w; x += 4) {
				const uint32_t v0 = s32[0];
				const uint32_t v1 = s32[1];

				d32[0] = ((v0 & 0x001f001fu) << 11) | ((v0 & 0x03e003e0u) << 1) | ((v0 & 0x7c007c00u) >> 10);
				d32[1] = ((v1 & 0x001f001fu) << 11) | ((v1 & 0x03e003e0u) << 1) | ((v1 & 0x7c007c00u) >> 10);
				s32 += 2;
				d32 += 2;
			}
			for (; x + 1 < w; x += 2) {
				const uint32_t v = *s32++;

				*d32++ = ((v & 0x001f001fu) << 11) | ((v & 0x03e003e0u) << 1) | ((v & 0x7c007c00u) >> 10);
			}
		}
		for (; x < w; x++) {
			const uint32_t v = s[x];

			d[x] = (uint16_t)(((v & 0x1fu) << 11) | ((v & 0x03e0u) << 1) | ((v >> 10) & 0x1fu));
		}
	}
}

/* 24 bit pixels packed three bytes each into the 16 bit cells of VRAM */
static void repack_rgb24(const uint8_t *src, int32_t w, int32_t rows, int32_t step, uint16_t *dst)
{
	for (int32_t y = 0; y < rows; y++) {
		const uint8_t *s = src + (size_t)y * step * PSX_GPU_FB_STRIDE;
		uint16_t *d = dst + (size_t)y * w;

		for (int32_t x = 0; x < w; x++, s += 3) {
			d[x] = (uint16_t)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
		}
	}
}

/* ---- text ------------------------------------------------------------------ */

static void txt(int x, int y, int scale, uint32_t color, const char *s)
{
	nema_bind_src_tex((uintptr_t)font_mem, 128, 48, NEMA_A1, 16, NEMA_FILTER_PS);
	nema_set_tex_color(color);
	nema_set_blend_blit(NEMA_BL_SIMPLE);
	for (; *s != '\0'; s++, x += 8 * scale) {
		int c = (unsigned char)*s;

		if (c < 32 || c > 127) {
			c = '?';
		}
		if (c == ' ') {
			continue;
		}
		int i = c - 32;

		nema_blit_subrect_fit(x, y, 8 * scale, 8 * scale, (i & 15) * 8, (i >> 4) * 8, 8, 8);
	}
}

static void submit_wait(void)
{
	nema_cl_submit(&cl);
	(void)nema_cl_wait(&cl);
	if (nema_port_hung()) {
		gpu_recover();
	}
}

static bool probe_abgr1555(void)
{
	/* red, green, blue, white (with the mask bit) as PSX pixels, and as RGB565 */
	static const uint16_t psx[4] = {0x001f, 0x03e0, 0x7c00, 0xffff};
	static const uint16_t rgb[4] = {0xf800, 0x07e0, 0x001f, 0xffff};

	/* the exact format, or the one with red and blue swapped read into a frame
	   buffer that is written swapped as well - both give the panel RGB565. The
	   first line is the control: a plain RGB565 copy, to show the probe works. */
	static const struct {
		nema_tex_format_t src, dst;
		const uint16_t *px;
		const char *name;
	} cand[] = {
		{NEMA_RGB565, NEMA_RGB565, rgb, "RGB565 -> RGB565 (control)"},
		{NEMA_ABGR1555, NEMA_RGB565, psx, "ABGR1555 -> RGB565"},
		{NEMA_ARGB1555, NEMA_BGR565, psx, "ARGB1555 -> BGR565"},
		{NEMA_ARGB1555, NEMA_RGB565, psx, "ARGB1555 -> RGB565 (swapped, for the record)"},
	};
	bool found = false;

	for (size_t c = 0; c < ARRAY_SIZE(cand); c++) {
		/* 8x8 textures, two columns per colour (the selftest's recipe) */
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				probe_src[y * 8 + x] = cand[c].px[x / 2];
				probe_dst[y * 8 + x] = 0x1234;
			}
		}
		nema_bind_dst_tex((uintptr_t)probe_dst, 8, 8, cand[c].dst, 16);
		nema_set_clip(0, 0, 8, 8);
		nema_bind_src_tex((uintptr_t)probe_src, 8, 8, cand[c].src, 16, NEMA_FILTER_PS);
		nema_set_blend_blit(NEMA_BL_SRC);
		nema_blit(0, 0);
		submit_wait();

		bool ok = true;
		uint16_t got[4];

		for (int i = 0; i < 4; i++) {
			/* the 5 bit green becomes 6 bits by a shift or by replication: both are right */
			got[i] = probe_dst[3 * 8 + i * 2];
			ok = ok && ((got[i] == rgb[i]) || ((got[i] | 0x0020) == (rgb[i] | 0x0020)));
		}
		LOG_INF("%s: %04x %04x %04x %04x -> %04x %04x %04x %04x: %s", cand[c].name, cand[c].px[0],
			cand[c].px[1], cand[c].px[2], cand[c].px[3], got[0], got[1], got[2], got[3],
			ok ? "right" : "wrong");
		if (ok && (c == 1 || c == 2) && !found) {
			direct_src_fmt = cand[c].src;
			direct_dst_fmt = cand[c].dst;
			found = true;
		}
	}
	LOG_INF("%s", found ? "the GPU reads VRAM directly" : "no usable 1555 source format: the CPU repacks");

	/* and as a destination: could the GPU draw straight into VRAM? An RGB565 source
	   of red, green, blue, white; what lands in a 16 bit destination of each layout */
	static const struct {
		nema_tex_format_t dst;
		const char *name;
	} dcand[] = {
		{NEMA_ABGR1555, "dst ABGR1555 (PSX layout)"},
		{NEMA_ARGB1555, "dst ARGB1555 (PSX with R/B swapped)"},
		{NEMA_BGRA5551, "dst BGRA5551"},
		{NEMA_RGBA5551, "dst RGBA5551"},
	};

	for (size_t c = 0; c < ARRAY_SIZE(dcand); c++) {
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				probe_src[y * 8 + x] = rgb[x / 2];
				probe_dst[y * 8 + x] = 0x1234;
			}
		}
		nema_bind_dst_tex((uintptr_t)probe_dst, 8, 8, dcand[c].dst, 16);
		nema_set_clip(0, 0, 8, 8);
		nema_bind_src_tex((uintptr_t)probe_src, 8, 8, NEMA_RGB565, 16, NEMA_FILTER_PS);
		nema_set_blend_blit(NEMA_BL_SRC);
		nema_blit(0, 0);
		submit_wait();
		LOG_INF("%s: %04x %04x %04x %04x", dcand[c].name, probe_dst[3 * 8 + 0], probe_dst[3 * 8 + 2],
			probe_dst[3 * 8 + 4], probe_dst[3 * 8 + 6]);
	}
	return found;
}


/* ---- can the GPU draw PSX primitives fast enough? ----------------------------------
   Small triangles into a 320x240 RGBA5551 target in PSRAM, like a game's: flat,
   Gouraud, and 4 bit palette textured out of a VRAM-like page. The rates decide
   whether the rasterizer moves to the GPU (a PS1 scene is 20-40 thousand a second). */

static uint32_t bench_pal[16] __nocache __aligned(32);

static uint32_t xorshift(uint32_t *st)
{
	uint32_t x = *st;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*st = x;
	return x;
}

static void bench_prims(psx_gpu_t *gpu)
{
	enum { N = 2000, SZ = 24 };
	uint16_t *target = stage; /* 320x240, stride 640 */
	uint32_t rs = 0x1234567u;
	uint32_t t0, t_cpu, t_gpu;
	char line[200];
	size_t n = 0;

	for (int i = 0; i < 16; i++) {
		bench_pal[i] = 0xff000000u | ((uint32_t)(i * 16) << 16) | ((uint32_t)(255 - i * 16) << 8) | 0x55u;
	}
	/* something in the texture page */
	for (uint32_t i = 0; i < 128u * 256u / 2u; i++) {
		((uint32_t *)gpu->vram)[i] = xorshift(&rs);
	}
	sys_cache_data_flush_range(gpu->vram, 256u * 2048u);

	nema_bind_dst_tex((uintptr_t)target, 320, 240, NEMA_RGBA5551, 640);
	nema_set_clip(0, 0, 320, 240);

	/* flat */
	t0 = k_cycle_get_32();
	nema_set_blend_fill(NEMA_BL_SRC);
	for (int i = 0; i < N; i++) {
		int x = (int)(xorshift(&rs) % 300u), y = (int)(xorshift(&rs) % 220u);

		nema_fill_triangle(x, y, x + SZ, y + 3, x + 5, y + SZ, 0xff000000u | xorshift(&rs));
	}
	nema_cl_submit(&cl);
	t_cpu = k_cycle_get_32() - t0;
	(void)nema_cl_wait(&cl);
	t_gpu = k_cycle_get_32() - t0;
	n += snprintf(line + n, sizeof(line) - n, " flat %u/%u us", k_cyc_to_us_floor32(t_cpu), k_cyc_to_us_floor32(t_gpu));

	/* Gouraud */
	t0 = k_cycle_get_32();
	nema_enable_gradient(1);
	nema_set_blend_fill(NEMA_BL_SRC);
	for (int i = 0; i < N; i++) {
		float x = (float)(xorshift(&rs) % 300u), y = (float)(xorshift(&rs) % 220u);
		color_var_t c0 = {255.f, 0.f, 0.f, 255.f}, c1 = {0.f, 255.f, 0.f, 255.f}, c2 = {0.f, 0.f, 255.f, 255.f};

		nema_interpolate_tri_colors(x, y, x + SZ, y + 3, x + 5, y + SZ, &c0, &c1, &c2);
		nema_fill_triangle_f(x, y, x + SZ, y + 3, x + 5, y + SZ, 0xffffffffu);
	}
	nema_enable_gradient(0);
	nema_cl_submit(&cl);
	t_cpu = k_cycle_get_32() - t0;
	(void)nema_cl_wait(&cl);
	t_gpu = k_cycle_get_32() - t0;
	n += snprintf(line + n, sizeof(line) - n, " gouraud %u/%u us", k_cyc_to_us_floor32(t_cpu), k_cyc_to_us_floor32(t_gpu));

	/* textured, 4 bit palette out of a 256x256 page of VRAM (stride 2048) */
	t0 = k_cycle_get_32();
	platform_invalidate_cache();
	nema_bind_lut_tex((uintptr_t)gpu->vram, 256, 256, NEMA_L4, 2048, NEMA_FILTER_PS, (uintptr_t)bench_pal,
			  NEMA_BGRA8888);
	nema_set_blend_blit(NEMA_BL_SRC | NEMA_BLOP_LUT);
	for (int i = 0; i < N; i++) {
		float x = (float)(xorshift(&rs) % 300u), y = (float)(xorshift(&rs) % 220u);
		float u = (float)(xorshift(&rs) % 200u), v = (float)(xorshift(&rs) % 200u);

		nema_blit_tri_uv(x, y, 1.f, x + SZ, y + 3, 1.f, x + 5, y + SZ, 1.f, u, v, u + 32.f, v + 4.f, u + 6.f,
				 v + 32.f);
	}
	nema_cl_submit(&cl);
	t_cpu = k_cycle_get_32() - t0;
	(void)nema_cl_wait(&cl);
	t_gpu = k_cycle_get_32() - t0;
	n += snprintf(line + n, sizeof(line) - n, " lut4 %u/%u us", k_cyc_to_us_floor32(t_cpu), k_cyc_to_us_floor32(t_gpu));

	/* the same, with the texture page in the internal SRAM (the font buffer is too small: use the CL area's neighbour) */
	LOG_INF("GPU primitives, %d of %dpx (CPU/total):%s", N, SZ, line);
	memset(gpu->vram, 0, 256u * 2048u);
	sys_cache_data_flush_range(gpu->vram, 256u * 2048u);
}

void present_bench(psx_gpu_t *gpu)
{
	bench_prims(gpu);
}

void present_text(const char *line1, const char *line2)
{
	pending_wait();

	int fb = pick_free_fb();

	if (fb < 0) {
		return;
	}
	nema_bind_dst_tex((uintptr_t)fb_mem[fb], FB_W, FB_H, NEMA_RGB565, FB_W * 2);
	nema_set_clip(0, 0, FB_W, FB_H);
	nema_set_blend_fill(NEMA_BL_SRC);
	nema_clear(nema_rgba(6, 6, 16, 255));
	txt(FB_W / 2 - (int)strlen(line1) * 12, 200, 3, nema_rgba(255, 120, 40, 255), line1);
	txt(FB_W / 2 - (int)strlen(line2) * 8, 250, 2, nema_rgba(200, 200, 210, 255), line2);
	submit_wait();
	flip_queue(fb);
	clear_pending = NFB;
	last_scaled_w = -1;
}

/* ---- the picture ----------------------------------------------------------- */

void present_vblank(psx_gpu_t *gpu)
{
	pending_poll();

	if (!gpu->vram_dirty) {
		return;
	}
	if (pending_fb >= 0) {
		/* the GPU is still scaling the last one: a few milliseconds at most, and
		   skipping would drop this frame when the board is the bottleneck */
		st.skipped++;
		pending_wait();
	}

	/* one field per frame: only when the rows we show were just drawn */
	if ((gpu->skip_rows >= 0) && ((uint32_t)gpu->skip_rows != (gpu->disp_y & 1u))) {
		return;
	}

	int fb = pick_free_fb();

	if (fb < 0) {
		st.skipped++;
		return; /* stays dirty: next blank */
	}

	const uint32_t t0 = k_cycle_get_32();

	gpu->vram_dirty = 0;

	const uint32_t dirty_y0 = gpu->dirty_y0;
	const uint32_t dirty_y1 = gpu->dirty_y1;

	gpu->dirty_y0 = 0xffffu;
	gpu->dirty_y1 = 0;

	int32_t src_w = (int32_t)dmode_width(gpu);
	int32_t src_h = (int32_t)dmode_height(gpu);
	const int32_t is_24 = (gpu->display_mode >> 4) & 1;
	const uint32_t win_x = gpu->disp_x;
	const uint32_t win_y = gpu->disp_y;
	const bool display_off = (gpu->gpustat & 0x800000u) != 0u;

	if (src_w <= 0 || src_w > STAGE_W) {
		src_w = 320;
	}
	if (src_h <= 0 || src_h > STAGE_H) {
		src_h = 240;
	}
	if ((int32_t)win_y + src_h > PSX_GPU_FB_HEIGHT) {
		src_h = PSX_GPU_FB_HEIGHT - (int32_t)win_y;
	}
	if (src_h <= 0) {
		src_h = 1;
	}

	/* a field-drawing game: one field, parity of disp_y, scaled up */
	const int32_t row_step = (gpu->skip_rows >= 0 && src_h > 240) ? 2 : 1;

	src_h /= row_step;

	/* repack: all of it when the window changed, else the rows that were drawn */
	int32_t row0 = 0;
	int32_t row1 = src_h;

	if ((staged_x == win_x) && (staged_y == win_y) && (staged_w == src_w) && (staged_h == src_h) &&
	    (staged_24 == is_24) && (staged_step == row_step) && (dirty_y0 < dirty_y1)) {
		const int32_t first = ((int32_t)dirty_y0 - (int32_t)win_y) / row_step;
		const int32_t last = ((int32_t)dirty_y1 - (int32_t)win_y + row_step - 1) / row_step;

		if (dirty_y0 > win_y) {
			row0 = (first < src_h) ? first : src_h;
		}
		if (last < src_h) {
			row1 = (last > row0) ? last : row0;
		}
	}
	staged_x = win_x;
	staged_y = win_y;
	staged_w = src_w;
	staged_h = src_h;
	staged_24 = is_24;
	staged_step = row_step;

	/* 15 bit pictures go to the GPU straight from VRAM (an even x start keeps the
	   texture 4 byte aligned); 24 bit ones and a GPU without the format are repacked */
	const bool direct = direct_ok && !is_24 && ((win_x & 1u) == 0u);

	if (direct) {
		if (row1 > row0 && !display_off) {
			/* what gpu.c drew is in the data cache: the GPU reads the memory */
			const uint8_t *rows = (const uint8_t *)gpu->vram +
					      (size_t)(win_y + (uint32_t)(row0 * row_step)) * PSX_GPU_FB_STRIDE;

			sys_cache_data_flush_range((void *)rows,
						   (size_t)((row1 - row0) * row_step) * PSX_GPU_FB_STRIDE);
		}
	} else if (row1 > row0 && !display_off) {
		const uint16_t *src = gpu->vram + win_x + (size_t)win_y * PSX_GPU_FB_WIDTH;
		uint16_t *out = stage + (size_t)row0 * src_w;

		if (is_24) {
			repack_rgb24((const uint8_t *)src + (size_t)(row0 * row_step) * PSX_GPU_FB_STRIDE, src_w,
				     row1 - row0, row_step, out);
		} else {
			repack_bgr555(src + (size_t)(row0 * row_step) * PSX_GPU_FB_WIDTH, src_w, row1 - row0,
				      row_step, out);
		}
		sys_cache_data_flush_range(out, (size_t)(row1 - row0) * src_w * 2u);
	}

	const uint32_t t1 = k_cycle_get_32();

	/* 4:3 into the panel: 640x480 centred */
	int32_t scaled_h = FB_H;
	int32_t scaled_w = (FB_H * 4) / 3;
	const int32_t x_off = (FB_W - scaled_w) / 2;
	const int32_t y_off = 0;

	if (scaled_w != last_scaled_w || scaled_h != last_scaled_h) {
		last_scaled_w = scaled_w;
		last_scaled_h = scaled_h;
		clear_pending = NFB;
		LOG_INF("window %dx%d%s at (%u,%u) mode %03x -> %dx%d", src_w, src_h * row_step,
			is_24 ? " 24bpp" : "", win_x, win_y, gpu->display_mode, scaled_w, scaled_h);
	}

	nema_bind_dst_tex((uintptr_t)fb_mem[fb], FB_W, FB_H, NEMA_RGB565, FB_W * 2);
	nema_set_clip(0, 0, FB_W, FB_H);
	if (clear_pending > 0) {
		clear_pending--;
		nema_set_blend_fill(NEMA_BL_SRC);
		nema_clear(nema_rgba(0, 0, 0, 255));
	}
	if (display_off) {
		nema_set_blend_fill(NEMA_BL_SRC);
		nema_fill_rect(x_off, y_off, scaled_w, scaled_h, nema_rgba(0, 0, 0, 255));
	} else {
		/* the staging picture was written by the CPU: the GPU's own cache must not keep old lines */
		platform_invalidate_cache();
		const bool whole = ((scaled_w % src_w) == 0) && ((scaled_h % src_h) == 0);

		if (direct) {
			nema_bind_dst_tex((uintptr_t)fb_mem[fb], FB_W, FB_H, direct_dst_fmt, FB_W * 2);
			nema_bind_src_tex((uintptr_t)(gpu->vram + win_x + (size_t)win_y * PSX_GPU_FB_WIDTH), src_w,
					  src_h, direct_src_fmt, PSX_GPU_FB_STRIDE * row_step,
					  whole ? NEMA_FILTER_PS : NEMA_FILTER_BL);
		} else {
			nema_bind_src_tex((uintptr_t)stage, src_w, src_h, NEMA_RGB565, src_w * 2,
					  whole ? NEMA_FILTER_PS : NEMA_FILTER_BL);
		}
		nema_set_blend_blit(NEMA_BL_SRC);
		nema_blit_subrect_fit(x_off, y_off, scaled_w, scaled_h, 0, 0, src_w, src_h);
	}
	nema_cl_submit(&cl);
	fb_st[fb] = FB_BUSY;
	pending_fb = fb;
	pending_id = cl.submission_id;
	pending_since = k_cycle_get_32();

	st.frames++;
	st.repack_us = k_cyc_to_us_floor32(t1 - t0);
	st.cpu_us_acc += k_cyc_to_us_floor32(pending_since - t0);
	st.src_w = (uint32_t)src_w;
	st.src_h = (uint32_t)src_h;
	st.mode = gpu->display_mode;
}

void present_get_stats(struct present_stats *out)
{
	*out = st;
}

void present_stats_reset(void)
{
	st.cpu_us_acc = 0;
}
