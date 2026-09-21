#ifndef BUS_FAST_H
#define BUS_FAST_H

/*
    Inlineable fast paths for the hot memory regions of the PSX bus
    (main RAM, scratchpad and BIOS ROM).

    The generic dispatcher in bus.c walks a table of 16 devices for every
    single access, which is far too expensive for an interpreter running on
    an MCU. These helpers handle the three regions that account for
    virtually every CPU access and fall back to the generic path for I/O.

    Semantics (address masking, mirroring, access_cycles) are identical to
    the generic psx_bus_read / psx_bus_write functions.
*/

#include <stdint.h>

#include "bus.h"
#include "bus_init.h"

#define PSX_BUS_FAST static inline __attribute__((always_inline))

/* Recompiler hooks (declared here to avoid pulling the JIT headers into every
   translation unit that does guest memory accesses). */
extern uint8_t g_psx_jit_code_pages[];
void psx_jit_invalidate(uint32_t addr, uint32_t size);

/* One flag per 256 bytes of guest RAM: is there translated code in there? (The
   same number is in jit/jit_translate.h, for the stores the recompiler emits.) */
#define PSX_JIT_CODE_FLAG_SHIFT 8u

/* Guest RAM write: only where a block was compiled from needs invalidating,
   which is a single byte test in the common case. Stores are aligned, so one
   never reaches into the next 256 bytes. */
PSX_BUS_FAST void psx_bus_fast_note_write(uint32_t phys, uint32_t size)
{
    if (g_psx_jit_code_pages[(phys & 0x1fffffu) >> PSX_JIT_CODE_FLAG_SHIFT])
        psx_jit_invalidate(phys, size);
}

#define PSX_RAM_FAST_SIZE 0x200000u
#define PSX_SPAD_FAST_BASE 0x1f800000u
#define PSX_SPAD_FAST_SIZE 0x400u
#define PSX_BIOS_FAST_BASE 0x1fc00000u

/* Same result as g_psx_bus_region_mask_table[addr >> 29] without the load */
PSX_BUS_FAST uint32_t psx_bus_fast_mask(uint32_t addr)
{
    uint32_t seg = addr >> 29;

    if (seg == 4) /* KSEG0 */
        return addr & 0x7fffffff;

    if (seg == 5) /* KSEG1 */
        return addr & 0x1fffffff;

    return addr;
}

PSX_BUS_FAST uint32_t psx_bus_fast_take_cycles(psx_bus_t *bus)
{
    uint32_t cycles = bus->access_cycles;

    bus->access_cycles = 0;

    return cycles;
}

PSX_BUS_FAST uint32_t psx_bus_fast_read32(psx_bus_t *bus, uint32_t addr)
{
    uint32_t a = psx_bus_fast_mask(addr);

    if (a < PSX_RAM_FAST_SIZE)
    {
        bus->access_cycles = bus->ram->bus_delay;

        return *(const uint32_t *)(bus->ram->buf + a);
    }

    if ((a - PSX_BIOS_FAST_BASE) < bus->bios->io_size)
    {
        bus->access_cycles = bus->bios->bus_delay;

        return *(const uint32_t *)(bus->bios->buf + (a - PSX_BIOS_FAST_BASE));
    }

    if ((a - PSX_SPAD_FAST_BASE) < PSX_SPAD_FAST_SIZE)
    {
        bus->access_cycles = bus->scratchpad->bus_delay;

        return *(const uint32_t *)(bus->scratchpad->buf + (a - PSX_SPAD_FAST_BASE));
    }

    return psx_bus_read32(bus, addr);
}

PSX_BUS_FAST uint16_t psx_bus_fast_read16(psx_bus_t *bus, uint32_t addr)
{
    uint32_t a = psx_bus_fast_mask(addr);

    if (a < PSX_RAM_FAST_SIZE)
    {
        bus->access_cycles = bus->ram->bus_delay;

        return *(const uint16_t *)(bus->ram->buf + a);
    }

    if ((a - PSX_SPAD_FAST_BASE) < PSX_SPAD_FAST_SIZE)
    {
        bus->access_cycles = bus->scratchpad->bus_delay;

        return *(const uint16_t *)(bus->scratchpad->buf + (a - PSX_SPAD_FAST_BASE));
    }

    if ((a - PSX_BIOS_FAST_BASE) < bus->bios->io_size)
    {
        bus->access_cycles = bus->bios->bus_delay;

        return *(const uint16_t *)(bus->bios->buf + (a - PSX_BIOS_FAST_BASE));
    }

    return psx_bus_read16(bus, addr);
}

PSX_BUS_FAST uint8_t psx_bus_fast_read8(psx_bus_t *bus, uint32_t addr)
{
    uint32_t a = psx_bus_fast_mask(addr);

    if (a < PSX_RAM_FAST_SIZE)
    {
        bus->access_cycles = bus->ram->bus_delay;

        return bus->ram->buf[a];
    }

    if ((a - PSX_SPAD_FAST_BASE) < PSX_SPAD_FAST_SIZE)
    {
        bus->access_cycles = bus->scratchpad->bus_delay;

        return bus->scratchpad->buf[a - PSX_SPAD_FAST_BASE];
    }

    if ((a - PSX_BIOS_FAST_BASE) < bus->bios->io_size)
    {
        bus->access_cycles = bus->bios->bus_delay;

        return bus->bios->buf[a - PSX_BIOS_FAST_BASE];
    }

    return psx_bus_read8(bus, addr);
}

PSX_BUS_FAST void psx_bus_fast_write32(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    uint32_t a = psx_bus_fast_mask(addr);

    if (a < PSX_RAM_FAST_SIZE)
    {
        bus->access_cycles = bus->ram->bus_delay;

        *(uint32_t *)(bus->ram->buf + a) = value;

        psx_bus_fast_note_write(a, 4);

        return;
    }

    if ((a - PSX_SPAD_FAST_BASE) < PSX_SPAD_FAST_SIZE)
    {
        bus->access_cycles = bus->scratchpad->bus_delay;

        *(uint32_t *)(bus->scratchpad->buf + (a - PSX_SPAD_FAST_BASE)) = value;

        return;
    }

    psx_bus_write32(bus, addr, value);
}

PSX_BUS_FAST void psx_bus_fast_write16(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    uint32_t a = psx_bus_fast_mask(addr);

    if (a < PSX_RAM_FAST_SIZE)
    {
        bus->access_cycles = bus->ram->bus_delay;

        *(uint16_t *)(bus->ram->buf + a) = (uint16_t)value;

        psx_bus_fast_note_write(a, 2);

        return;
    }

    if ((a - PSX_SPAD_FAST_BASE) < PSX_SPAD_FAST_SIZE)
    {
        bus->access_cycles = bus->scratchpad->bus_delay;

        *(uint16_t *)(bus->scratchpad->buf + (a - PSX_SPAD_FAST_BASE)) = (uint16_t)value;

        return;
    }

    psx_bus_write16(bus, addr, value);
}

PSX_BUS_FAST void psx_bus_fast_write8(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    uint32_t a = psx_bus_fast_mask(addr);

    if (a < PSX_RAM_FAST_SIZE)
    {
        bus->access_cycles = bus->ram->bus_delay;

        bus->ram->buf[a] = (uint8_t)value;

        psx_bus_fast_note_write(a, 1);

        return;
    }

    if ((a - PSX_SPAD_FAST_BASE) < PSX_SPAD_FAST_SIZE)
    {
        bus->access_cycles = bus->scratchpad->bus_delay;

        bus->scratchpad->buf[a - PSX_SPAD_FAST_BASE] = (uint8_t)value;

        return;
    }

    psx_bus_write8(bus, addr, value);
}

#endif
