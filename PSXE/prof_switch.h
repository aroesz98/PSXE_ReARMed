#ifndef PSX_PROF_SWITCH_H
#define PSX_PROF_SWITCH_H

/* The switch of the profiling build, on its own so that FreeRTOSConfig.h can see
   it too: the PC sampler of that build hangs on the tick hook. */
#ifndef PSX_PROFILE
#define PSX_PROFILE 0
#endif

#endif
