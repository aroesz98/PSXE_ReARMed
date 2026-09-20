/*
    Game picker shown before the emulator starts.
*/

#ifndef PSXE_MENU_H
#define PSXE_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
    Scans the card for disc images and lets the user pick one, by touch or with
    the controller. Blocks until something is chosen.

    Writes the chosen path into `path` and returns 1. Returns 0 when the card
    holds no images at all, in which case the caller should fall back to its
    built in path.
*/
int32_t psxe_menu_pick(char *path, uint32_t path_size);

#ifdef __cplusplus
}
#endif

#endif
