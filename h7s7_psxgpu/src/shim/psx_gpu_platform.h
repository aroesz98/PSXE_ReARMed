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

/* no interrupt controller: the CPU board has the timing */
#define psx_ic_irq(ic, irq) ((void)(ic))
enum { IC_VBLANK = 0x001, IC_GPU = 0x002 };

/* the profiler (prof.h) and a memory check, in psx_shim.c */
void psxgpu_prof_init(void);
void psxgpu_prof_report(void);
void psxgpu_mem_bench(void);

#endif /* PSX_GPU_PLATFORM_H_ */
