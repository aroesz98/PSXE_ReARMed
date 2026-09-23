#ifndef PSXE_OVERCLOCK_H
#define PSXE_OVERCLOCK_H

#include <stdint.h>

/*
    The core clock, in MHz. 600 is what the i.MX RT1050 is specified for and
    what BOARD_BootClockRUN sets up; anything above that is an overclock (see
    overclock.c): a multiple of 12 from 648 to 1296. The build scripts can
    override it: build.bat -Define PSXE_CPU_MHZ=600

    Measured with a properly powered board in Tekken 3's fights: 816 MHz
    25 fps 52 C, 912 MHz 91-98% of a PS1, 28-29 fps, 59 C; 984 MHz (1.575 V,
    the most the DCDC gives) was too much. 948 MHz runs at 1.55 V.
*/
#ifndef PSXE_CPU_MHZ
#define PSXE_CPU_MHZ 948
#endif

/*
    The core voltage in mV for an overclock, in 25 mV steps (the DCDC's), at
    most 1575; 0 takes it from the Teensy rule in overclock.c (1550 at 948 MHz).
    948 MHz is stable at 1550; at 1525 it ran for a while, then hung in a
    HardFault (imprecise bus error, CFSR 0x400) - too little voltage.
*/
#ifndef PSXE_CPU_MV
#define PSXE_CPU_MV 1550
#endif

/* After BOARD_BootClockRUN, before the scheduler starts: the core to
   PSXE_CPU_MHZ, its voltage first. Returns the core voltage in mV. */
uint32_t BOARD_SetCoreClock(uint32_t mhz);

/* The on-chip temperature sensor, started once; the core temperature in whole
   degrees Celsius (-1000 before the first measurement). */
void BOARD_TempStart(void);
int32_t BOARD_TempCelsius(void);

#endif
