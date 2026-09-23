/*
 * The PSX GPU's primitives on the NeoChrom: what gpu.c calls instead of
 * rasterizing them itself (PSX_GPU_EXTERNAL_RASTER). See nema_raster.c.
 */
#ifndef NEMA_RASTER_H_
#define NEMA_RASTER_H_

#include <stdint.h>

#include "gpu.h"

void psx_raster_init(void);

/* how the GPU reads a PSX texture, checked against known data at boot */
void psx_raster_selftest(psx_gpu_t *gpu);

/* GP0 primitives drawn by the NeoChrom and by gpu.c, compared (printed, and in g_tritest) */
void psx_raster_tritest(psx_gpu_t *gpu);

/* everything queued is drawn; the CPU may touch VRAM after this */
void psx_raster_sync(void);

/* the hooks gpu.c calls: 1 when the GPU took the primitive, 0 to rasterize it */
int psx_raster_triangle(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data);
int psx_raster_rect(psx_gpu_t *gpu, rect_data_t data);
int psx_raster_line(psx_gpu_t *gpu, vertex_t v0, vertex_t v1, uint32_t color, int transp);
int psx_raster_fill(psx_gpu_t *gpu, int x, int y, int w, int h, uint32_t color);

struct psx_raster_stats {
	uint32_t prims, flat, gouraud, textured, rects, lines;
	uint32_t tex_binds, palettes, flushes, hangs;
	uint32_t tex15;        /* 15 bit textures, whose red and blue come out swapped */
	uint32_t invalidations; /* times the GPU's read cache had to be dropped */
	uint32_t gpu_cyc;      /* cycles waiting for the GPU to drain the list */
	uint32_t cpu_cyc;      /* ... and building its commands */
	uint32_t bind_cyc;     /* ... of which binding textures and palettes */
};

void psx_raster_get_stats(struct psx_raster_stats *out);
void psx_raster_reset_stats(void);

#endif /* NEMA_RASTER_H_ */
