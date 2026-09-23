/*
 * What gpu.c needs from the platform when it is built on its own, here for
 * Zephyr on the STM32H7S7: where its code and state go, and how VRAM is got.
 */
#ifndef PSX_GPU_PLATFORM_H_
#define PSX_GPU_PLATFORM_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/linker/devicetree_regions.h>

/* the rasterizer runs from AXI SRAM, not from the external flash it boots from */
#define PSX_GPU_HOT __attribute__((section(".ramfunc")))
#define PSX_GPU_ITC __attribute__((section(".ramfunc")))
/* the innermost rasterizer in the 64 KB ITCM: zero wait states, no cache misses */
#define PSX_GPU_RAS __attribute__((section(".itcm")))

/* the GPU state in the DTCM: a NOLOAD region, which psx_gpu_create() clears */
#define PSX_GPU_DTCM_BSS __attribute__((section(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(dtcm)))))

/* the 1 MB of VRAM lives in the PSRAM; the "display off" buffer is not needed here */
void *psxgpu_vram_alloc(size_t size);
#define psx_gpu_alloc(size) psxgpu_vram_alloc(size)
#define psx_gpu_alloc_empty(size) ((void *)0)
#define psx_gpu_free(p) ((void)(p))

/*
    The drawing is done by the NeoChrom (src/nema_raster.c).

    VRAM keeps whatever the game puts in it, untouched: most of what a game
    uploads is not colour at all but packed 4 or 8 bit palette indices, and
    shuffling those bits would destroy every texture. What the GPU2D writes into
    VRAM, on the other hand, is RGBA5551 - the only 16 bit format it writes - so
    drawn pixels are in that format and uploaded ones in the PSX's BGR555. The
    two meet in three places, all of them handled:

      - the palette (CLUT) is uploaded colour, and is converted to the GPU's
        BGRA8888 where it is bound (nema_raster.c);
      - the picture on the panel is what the GPU drew, so the presenter reads it
        as RGBA5551;
      - a read back to the game (GP0 C0h) of a frame buffer is converted back.

    Which uploaded pixels are colour is told by where they land, pixel by
    pixel: inside the display window or the drawing area they are converted to
    RGBA5551, everywhere else (texture pages, palettes) they stay as they came.
    A read back converts the same pixels the other way (gpu_img_rects, gpu.c).

    What this leaves wrong: a 15 bit texture is read as RGBA5551 although it is
    BGR555 - its red and blue come out swapped.
*/
#define PSX_GPU_EXTERNAL_RASTER 1

#define PSX_PIX_FROM_PSX(p)                                                                    \
    ((uint16_t)((p) ? (((((p) & 0x1fu) << 11) | ((((p) >> 5) & 0x1fu) << 6) |                        \
                        ((((p) >> 10) & 0x1fu) << 1) | 1u))                                         \
                    : 0u))

#define PSX_PIX_TO_PSX(p)                                                                      \
    ((uint16_t)((((p) >> 11) & 0x1fu) | ((((p) >> 6) & 0x1fu) << 5) | ((((p) >> 1) & 0x1fu) << 10)))

/* the CPU reads VRAM the GPU2D may have written (GP0 80h, C0h): the data cache
   holds lines from before, which the PSRAM being write-through does not help */
void psxgpu_vram_cpu_read(const uint16_t *vram, uint32_t y, uint32_t rows);
#define PSX_VRAM_CPU_READ(gpu, y, rows) psxgpu_vram_cpu_read((gpu)->vram, (y), (rows))

/* no interrupt controller: the CPU board has the timing */
#define psx_ic_irq(ic, irq) ((void)(ic))
enum { IC_VBLANK = 0x001, IC_GPU = 0x002 };

/* the profiler (prof.h) and a memory check, in psx_shim.c */
void psxgpu_prof_init(void);
void psxgpu_prof_report(void);
void psxgpu_mem_bench(void);

#endif /* PSX_GPU_PLATFORM_H_ */
