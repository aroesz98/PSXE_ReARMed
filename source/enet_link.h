#ifndef ENET_LINK_H
#define ENET_LINK_H

/*
    Raw Ethernet on the EVKB (ENET + KSZ8081, RMII), for the link to the GPU
    board: whole frames in, whole frames out, no protocol stack, no interrupts.
    Everything is called from the emulator task; nothing here blocks.
*/

#include <stdbool.h>
#include <stdint.h>

#define ENET_LINK_BUSY (-1)  /* the transmit ring is full: try again shortly */
#define ENET_LINK_DOWN (-2)

/* pins, clocks, PHY reset and auto-negotiation, MAC; 0 on success */
int enet_link_init(const uint8_t mac[6]);

/* the PHY's link state (an MDIO read: call every ~100 ms, not more) */
bool enet_link_poll_phy(void);
bool enet_link_up(void);

/* a frame to the wire (Ethernet header included, no CRC); 0, or ENET_LINK_* */
int enet_link_send(const void *frame, uint32_t len);

/* the next received frame into buf; its length, 0 when none, <0 on a bad frame */
int enet_link_recv(uint8_t *buf, uint32_t max);

#endif
