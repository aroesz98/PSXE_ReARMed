#ifndef BUS_H
#define BUS_H

#include <stdint.h>

struct psx_bus_t;

typedef struct psx_bus_t psx_bus_t;

psx_bus_t *psx_bus_create(void);
void psx_bus_init(psx_bus_t *);
uint32_t psx_bus_read32(psx_bus_t *, uint32_t);
uint16_t psx_bus_read16(psx_bus_t *, uint32_t);
uint8_t psx_bus_read8(psx_bus_t *, uint32_t);
void psx_bus_write32(psx_bus_t *, uint32_t, uint32_t);
void psx_bus_write16(psx_bus_t *, uint32_t, uint32_t);
void psx_bus_write8(psx_bus_t *, uint32_t, uint32_t);

/* Set by every write that is not plain memory, i.e. by every device register
   write. A register write is how the guest arms something - a timer target, a
   controller transfer, a DMA - so whoever planned ahead on the devices' next
   deadlines has to plan again. Cleared by whoever acts on it. */
extern uint32_t g_psx_bus_io_written;
uint32_t psx_bus_get_access_cycles(psx_bus_t *);
void psx_bus_destroy(psx_bus_t *);
void psx_bus_force_reset_singleton(void); // For testing only

#endif