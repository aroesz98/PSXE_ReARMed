/*
    Sound out of the EVKB's headphone jack (J12): the WM8960 codec, controlled
    over LPI2C1, fed over SAI1 as I2S - see audio_out.c.
*/
#ifndef PSXE_AUDIO_OUT_H
#define PSXE_AUDIO_OUT_H

#include <stdint.h>

#define AUDIO_OUT_RATE 44100u /* what the PSX SPU makes: no rate conversion */

/* Clocks, pins, codec, SAI and DMA; the output starts playing silence.
   0 on success, else the step that failed (the rest of the emulator runs
   either way, only without sound). */
int32_t audio_out_init(void);

/* count stereo frames (left, right, left, ...) from the emulator; what does not
   fit is dropped. Called from the emulator task only. */
void audio_out_push(const int16_t *frames, uint32_t count);

/* frames waiting to be played, and how many times since the last call the
   output ran dry (the emulation was behind) - for the status on screen */
uint32_t audio_out_queued(void);
uint32_t audio_out_underruns(void);

/* 1 when audio_out_init succeeded */
int32_t audio_out_ok(void);

#endif
