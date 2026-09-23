#ifndef PSXE_GPU_SWITCH_H
#define PSXE_GPU_SWITCH_H

/*
    Where the picture is made.

    1: the Ethernet link to the STM32H7S78-DK (h7s7_psxgpu) is compiled in, and
       the choice is made at run time: while the GPU board answers, every
       GP0/GP1 word goes to it (PSXE/link/gpu_remote.c) and it holds the VRAM,
       rasterizes and shows the frame on its own panel; this board keeps the
       GPU timing (blanks, GPUSTAT, fields), the DMA and the display registers.
       Without the GPU board (no cable, no answer, link lost) the picture is
       made here, with PSXE/dev/gpu.c and the LCD - and the two are switched
       in either direction whenever the link comes or goes, at a command
       boundary, with the whole VRAM sent over when the GPU board takes over.
       g_gpu_remote (gpu_remote.h) says which is active.
    0: only the local rasterizer and the LCD are built.

    The build scripts can override it: build.bat -Define PSXE_GPU_REMOTE=0
*/
#ifndef PSXE_GPU_REMOTE
#define PSXE_GPU_REMOTE 1
#endif

/* 1: send the GP0 stream and let the GPU board rasterize, instead of sending it
   finished pictures (see gpu_remote.h). For measuring; the hybrid is faster. */
#ifndef PSXE_GPU_REMOTE_CMD
#define PSXE_GPU_REMOTE_CMD 0
#endif

#endif
