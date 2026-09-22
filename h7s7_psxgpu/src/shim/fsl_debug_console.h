/* The emulator sources print through the NXP debug console; here that is printk. */
#ifndef FSL_DEBUG_CONSOLE_SHIM_H_
#define FSL_DEBUG_CONSOLE_SHIM_H_

#include <zephyr/sys/printk.h>

#define PRINTF printk

#endif
