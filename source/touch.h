/*
    GT911 touch panel on LPI2C1, as fitted to the RK043FN66HS-CTG panel of the
    RT1050-EVKB. See touch.c for the pins.
*/

#ifndef PSXE_TOUCH_H
#define PSXE_TOUCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the controller. Returns non zero when it answered. */
int32_t psxe_touch_init(void);

/*
    Current contact, in panel pixels (0..479, 0..271). Returns 1 while a finger
    is down, 0 otherwise; x and y keep their previous values when it returns 0.
*/
int32_t psxe_touch_read(int32_t *x, int32_t *y);

#ifdef __cplusplus
}
#endif

#endif
