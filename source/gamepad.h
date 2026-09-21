/*
    Gamepad input for the PSX emulator, received from the ESP32 Bluetooth bridge.

    See gamepad.c for the wiring and the frame format.
*/

#ifndef PSXE_GAMEPAD_H
#define PSXE_GAMEPAD_H

#include <stdint.h>

#include "dev/pad.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
    Button bit positions as the bridge sends them - the DualSense layout, which
    is what the ESP32 reads off the wire. The picker uses these before the
    emulated pad exists.
*/
enum
{
    PSXE_DS_DPAD_UP = 0,
    PSXE_DS_DPAD_RIGHT,
    PSXE_DS_DPAD_DOWN,
    PSXE_DS_DPAD_LEFT,
    PSXE_DS_SQUARE,
    PSXE_DS_CROSS,
    PSXE_DS_CIRCLE,
    PSXE_DS_TRIANGLE,
    PSXE_DS_L1,
    PSXE_DS_R1,
    PSXE_DS_L2,
    PSXE_DS_R2,
    PSXE_DS_CREATE,
    PSXE_DS_OPTIONS,
    PSXE_DS_L3,
    PSXE_DS_R3,
    PSXE_DS_PS,
};

/*
    Brings up the link. The emulated pad can be attached later with
    psxe_gamepad_bind, which is what the boot sequence does: the picker runs on
    the raw button state before the emulator exists.
*/
void psxe_gamepad_init(void);

/* Sends everything that arrives to this controller slot; NULL detaches. */
void psxe_gamepad_bind(psx_pad_t *pad, int32_t slot);

/* Last button state the bridge sent, in the bit layout above. */
uint32_t psxe_gamepad_raw_buttons(void);

/*
    Applies whatever the bridge last sent. Cheap when nothing arrived, so it can
    be called from the emulation loop as often as convenient; once per emulated
    frame is enough, since that is how often a game reads the pad.
*/
void psxe_gamepad_poll(void);

/* Unattended test runs: the picker starts the first image whose name contains
   PSXE_AUTOTEST_GAME and the pad taps the confirm button every few seconds, so
   a build can be driven into a game without anyone at the board. Never enabled
   in a build that is meant to be played. */
#ifndef PSXE_AUTOTEST
#define PSXE_AUTOTEST 0
#endif

/* Unattended boot soak: restart this many seconds into the emulation (0: never) */
#ifndef PSXE_AUTOTEST_REBOOT_S
#define PSXE_AUTOTEST_REBOOT_S 0
#endif

#ifndef PSXE_AUTOTEST_GAME
#define PSXE_AUTOTEST_GAME "final"
#endif

/* Non zero once at least one valid frame has arrived. */
int32_t psxe_gamepad_is_connected(void);

/* Frames received and frames dropped (bad checksum or lost bytes). */
void psxe_gamepad_get_stats(uint32_t *frames, uint32_t *errors);

#ifdef __cplusplus
}
#endif

#endif
