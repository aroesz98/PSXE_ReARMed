#ifndef PSXE_GPU_SWITCH_H
#define PSXE_GPU_SWITCH_H

/*
    Where the picture is made.

    1: on the STM32H7S78-DK (h7s7_psxgpu), over Ethernet. This board keeps the
       GPU timing (blanks, GPUSTAT, fields), the DMA and the display registers,
       and sends every GP0/GP1 word to the GPU board (PSXE/link/gpu_remote.c),
       which holds the VRAM, rasterizes and shows the frame on its own panel.
       The LCD of this board is not used for the game then.
    0: here, with PSXE/dev/gpu.c and the LCD (PSXE/frontend/screen.c).

    The build scripts can override it: build.bat -Define PSXE_GPU_REMOTE=0
*/
#ifndef PSXE_GPU_REMOTE
#define PSXE_GPU_REMOTE 1
#endif

#endif
