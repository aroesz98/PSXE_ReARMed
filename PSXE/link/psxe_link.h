/*
    The link between the two boards.

    The emulator runs on the i.MX RT1050 (the "CPU board"); the GPU commands it
    would rasterize itself go over Ethernet to the STM32H7S78-DK (the "GPU
    board"), which keeps the VRAM, rasterizes - the same gpu.c, later the
    NeoChrom - and drives its own panel. Raw Ethernet frames with an EtherType
    of our own, point to point: no IP, no ARP, nothing to configure.

    A frame is the 14 byte Ethernet header, a 4 byte link header and records.
    Every record is a 32 bit header - type, flags, payload length in words -
    followed by its payload. Records never straddle frames, so a frame can be
    parsed on its own. Everything is little endian, as both cores are.

      CPU -> GPU
        GP0     the words a game writes to GP0 (commands, arguments, uploads),
                in order. BOUNDARY: the first word starts a command - where a
                receiver that lost a frame can pick the stream up again.
        GP1     one GP1 word
        VBLANK  the CPU board's GPU timing reached the vertical blank: the field
                that goes on screen now (bit 0) and a frame counter (bits 8..)
        RESET   the GPU is (re)initialised: forget everything
      GPU -> CPU
        STATUS  flow control and health: how many payload bytes of GP0/GP1
                records have been consumed in all, the size of the receive
                ring, frames presented, errors
        VRAM    VRAM -> CPU data (GP0 C0h): the words GPUREAD returns, in order

    Flow control: the GPU board says how much it has consumed, the CPU board
    knows how much it has sent, and never has more than PSXE_LINK_WINDOW bytes
    in flight - what the receive ring can hold with room to spare. A STATUS
    record comes with every so many consumed bytes, and every so often anyway,
    so that the CPU board knows the GPU board is alive.
*/
#ifndef PSXE_LINK_H
#define PSXE_LINK_H

#include <stdint.h>

#define PSXE_LINK_ETHERTYPE 0x88B5u /* IEEE 802 local experimental */
#define PSXE_LINK_VERSION 1u

/* locally administered addresses: "PSX" + which board */
#define PSXE_LINK_MAC_CPU {0x02u, 0x50u, 0x53u, 0x58u, 0x43u, 0x50u}
#define PSXE_LINK_MAC_GPU {0x02u, 0x50u, 0x53u, 0x58u, 0x47u, 0x50u}

#define PSXE_LINK_ETH_HDR 14u
#define PSXE_LINK_HDR 4u
#define PSXE_LINK_MTU 1500u                                                 /* Ethernet payload */
#define PSXE_LINK_FRAME_MAX (PSXE_LINK_ETH_HDR + PSXE_LINK_MTU)             /* 1514 */
#define PSXE_LINK_PAYLOAD_WORDS ((PSXE_LINK_MTU - PSXE_LINK_HDR) / 4u)      /* 374 */

/* the receive ring of the GPU board, and how much of it the CPU board may fill */
#define PSXE_LINK_RING_BYTES (512u * 1024u)
#define PSXE_LINK_WINDOW (PSXE_LINK_RING_BYTES - 64u * 1024u)

/* link header: version, flags, sequence number of the frame (per direction) */
#define PSXE_LINK_HDR_VERSION(p) ((p)[0])
#define PSXE_LINK_HDR_FLAGS(p) ((p)[1])
#define PSXE_LINK_HDR_SEQ(p) ((uint16_t)((p)[2] | ((p)[3] << 8)))

/* record header */
#define PSXE_REC_TYPE(h) ((h) >> 28)
#define PSXE_REC_FLAGS(h) (((h) >> 24) & 0xfu)
#define PSXE_REC_LEN(h) ((h) & 0xffffffu)
#define PSXE_REC_HDR(type, flags, len) (((uint32_t)(type) << 28) | ((uint32_t)(flags) << 24) | ((uint32_t)(len) & 0xffffffu))

enum
{
    PSXE_REC_GP0 = 1,
    PSXE_REC_GP1 = 2,
    PSXE_REC_VBLANK = 3,
    PSXE_REC_RESET = 4,

    PSXE_REC_STATUS = 8,
    PSXE_REC_VRAM = 9
};

#define PSXE_REC_GP0_BOUNDARY 1u /* flag: the record starts at a command boundary */

/* VBLANK payload */
#define PSXE_VBLANK_FIELD(w) ((w) & 1u)
#define PSXE_VBLANK_FRAME(w) ((w) >> 8)

/* STATUS payload: words */
enum
{
    PSXE_STATUS_CONSUMED = 0, /* GP0/GP1/VBLANK/RESET record bytes consumed, in all */
    PSXE_STATUS_FLAGS = 1,
    PSXE_STATUS_RING = 2,     /* receive ring size in bytes */
    PSXE_STATUS_PRESENTED = 3,
    PSXE_STATUS_ERRORS = 4,   /* lost frames, parser resyncs, ring overruns */
    PSXE_STATUS_BOOT = 5,     /* the GPU board's boot id: a change means it started over */
    PSXE_STATUS_WORDS = 6
};

#define PSXE_STATUS_FLAG_READY 1u   /* the GPU board is initialised and listening */
#define PSXE_STATUS_FLAG_RESYNC 2u  /* it lost frames and waits for a command boundary */

#endif
