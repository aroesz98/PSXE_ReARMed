/*
 * From the PSX VRAM to the panel: the display window is repacked to RGB565,
 * the NeoChrom scales it into an 800x480 frame buffer, the LTDC shows it.
 */
#ifndef PRESENT_H_
#define PRESENT_H_

#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"

int present_init(void);

/* the one command list everything on this board draws through */
struct nema_cmdlist_t_;
struct nema_cmdlist_t_ *present_cl(void);

/* a boot-time measurement: how fast the GPU draws PSX sized primitives (needs the VRAM) */
void present_bench(psx_gpu_t *gpu);

/* the vertical blank of the CPU board's GPU timing: show the picture if it changed */
void present_vblank(psx_gpu_t *gpu);

/*
 * The hybrid: the CPU board rasterized the picture and sends it row by row.
 * present_rows() repacks what arrived straight out of the receive buffer - the
 * pixels are never copied anywhere else - and present_show() puts the finished
 * picture on the panel.
 */
void present_rows(const void *px, uint32_t w, uint32_t row, uint32_t rows, uint32_t stride,
		  uint32_t flags);
void present_show(uint32_t w, uint32_t h, uint32_t flags, uint32_t mode);

/* a message instead of the game (the link is not up yet) */
void present_text(const char *line1, const char *line2);

struct present_stats {
	uint32_t frames;        /* presented */
	uint32_t skipped;       /* dirty, but the flip slot was busy */
	uint32_t repack_us;     /* last present: CPU repack   */
	uint32_t gpu_us;        /* last present: GPU scale    */
	uint32_t src_w, src_h;  /* what is being shown */
	uint32_t mode;          /* GP1(08) */
	uint32_t cpu_us_acc;    /* CPU time in presents since the last reset */
	uint32_t repack_acc;    /* cycles repacking the picture being received */
};

void present_get_stats(struct present_stats *st);
void present_stats_reset(void);

#endif /* PRESENT_H_ */
