#ifndef PSXE_GPU_REMOTE_H
#define PSXE_GPU_REMOTE_H

/*
    The GPU on the other board (PSXE/gpu_switch.h, PSXE_GPU_REMOTE).

    gpu.c calls these in place of its own command parser and rasterizer: the
    GP0/GP1 words go over the Ethernet link to the STM32H7S78-DK, which runs the
    very same gpu.c on them. What stays here is what the game can read back:
    GPUSTAT (the texture page and environment bits are shadowed from the GP0
    stream), the GP1(10h) information words (gpu.c keeps them itself) and the
    VRAM -> CPU transfers (their data comes back over the link).
*/

#include <stdint.h>

#include "gpu.h"

/* psx_gpu_init(): fresh state; a RESET goes to the GPU board once it listens */
void gpu_remote_reset(psx_gpu_t *gpu);

/* a GP0 word: forwarded; returns E1h..E6h when the word is one of those
   environment commands, which gpu.c then applies to its own state, 0 otherwise */
uint32_t gpu_remote_gp0(psx_gpu_t *gpu, uint32_t word);

/* rows of a CPU -> VRAM upload straight from guest RAM (psx_gpu_write_bulk):
   the number of words taken, 0 when the stream is not inside such an upload */
uint32_t gpu_remote_gp0_bulk(psx_gpu_t *gpu, const uint32_t *src, uint32_t words);

/* a GP1 word: forwarded (gpu.c handles the display state locally as well) */
void gpu_remote_gp1(psx_gpu_t *gpu, uint32_t word);

/* VRAM -> CPU (GP0 C0h): words still owed, and the next one (waits for the link) */
uint32_t gpu_remote_reading(void);
uint32_t gpu_remote_read_vram(psx_gpu_t *gpu);

/* the vertical blank: the frame boundary goes out, the link is serviced */
void gpu_remote_vblank(psx_gpu_t *gpu, uint32_t field);

/* frames the GPU board reports as shown; a line of link statistics on the UART */
uint32_t gpu_remote_presented(void);
void gpu_remote_report(void);

#endif
