#ifndef PSXE_SOUND_SWITCH_H
#define PSXE_SOUND_SWITCH_H

/*
    How much of the sound hardware is emulated. This board has no audio output,
    so no sound is ever heard; what is left to choose is how much of the SPU the
    games can still see:

      0  nothing beyond its registers and its sound RAM: a voice that is keyed
         on never plays and never ends. Saves psx_spu_update, some 2% of the
         core in a Final Fantasy VII battle - but a game that waits for a sound
         to finish, or streams its music through the SPU interrupt, waits for
         ever: Need for Speed II in its menu.
      1  (default) the voices run as far as a game can see them - envelope,
         ENDX, the SPU interrupt - without a sample being decoded or mixed
         (psx_spu_update, spu.c). Nothing that makes sound is built.
      2  sound is made as well: mixing, reverb, XA and CD audio decoding
         (psx_spu_get_sample, psx_cdrom_get_audio_samples) with their buffers,
         about 115 KB of SDRAM. For a board with an audio output, which has to
         call those for every sample and then drives the voices itself; here
         nothing does, and the voices stand still as with 0.
*/
#ifndef PSXE_SOUND
#define PSXE_SOUND 0
#endif

#endif
