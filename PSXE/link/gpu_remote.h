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

/*
    Two boards, one picture.

    g_gpu_remote: the GPU board is there and takes the picture. What it does
    with it depends on g_gpu_stream:

      0 (the hybrid, the default): this board rasterizes into its own VRAM - its
        memory is the faster one - and sends the finished picture over the link
        (gpu_remote_frame); the GPU board repacks it, scales it with the
        NeoChrom and shows it on its 800x480 panel. The LCD here stays dark, and
        this board is spared the repack and the PXP.
      1 (PSXE_GPU_REMOTE_CMD=1): the GP0/GP1 stream goes over instead and the
        GPU board rasterizes it as well. Slower for games with many small
        textured polygons (its VRAM is external PSRAM), kept for measuring.

    Both are 0 while there is no GPU board: everything happens here, on the LCD.
*/
extern int g_gpu_remote;
extern int g_gpu_stream;

/* how the drawing is divided: the share of the drawing area this board takes,
   0 to 256 (0 = the GPU board draws all of it). Only in the GP0 stream mode. */
extern int g_gpu_band;

/* the picture, or the rows of it that changed, to the GPU board. src points at
   the top left of the display window in VRAM and its rows are src_stride bytes
   apart - two VRAM rows for a game that draws one field of an interlaced
   picture per frame, so that only the field just finished goes over.
   row0/row1 are the rows to send, 0 .. h. Returns 0 when it went out. */
int gpu_remote_frame(const void *src, uint32_t src_stride, uint32_t w, uint32_t h, uint32_t row0,
                     uint32_t row1, uint32_t flags, uint32_t mode);

/* psx_gpu_init(): fresh state; a RESET goes to the GPU board once it listens */
void gpu_remote_reset(psx_gpu_t *gpu);

/* a GP0 word: forwarded; returns E1h..E6h when the word is one of those
   environment commands, which gpu.c then applies to its own state, 0 otherwise */
uint32_t gpu_remote_gp0(psx_gpu_t *gpu, uint32_t word);

/* rows of a CPU -> VRAM upload straight from guest RAM (psx_gpu_write_bulk):
   the number of words taken, 0 when the stream is not inside such an upload */
uint32_t gpu_remote_gp0_bulk(psx_gpu_t *gpu, const uint32_t *src, uint32_t words);

/* the local rasterizer took this many words of an upload in bulk: the stream
   follower must not lose count of them */
void gpu_remote_note_bulk(uint32_t words);

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
