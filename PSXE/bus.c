#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "bus_init.h"
#include "log.h"

#define PSX_BUS_DEVICE_TABLE(OP, ...)                            \
    OP(bios, PSX_BUS_DEVICE_BIOS, ##__VA_ARGS__)                \
    OP(ram, PSX_BUS_DEVICE_RAM, ##__VA_ARGS__)                  \
    OP(dma, PSX_BUS_DEVICE_DMA, ##__VA_ARGS__)                  \
    OP(exp1, PSX_BUS_DEVICE_EXP1, ##__VA_ARGS__)                \
    OP(exp2, PSX_BUS_DEVICE_EXP2, ##__VA_ARGS__)                \
    OP(mc1, PSX_BUS_DEVICE_MC1, ##__VA_ARGS__)                  \
    OP(mc2, PSX_BUS_DEVICE_MC2, ##__VA_ARGS__)                  \
    OP(mc3, PSX_BUS_DEVICE_MC3, ##__VA_ARGS__)                  \
    OP(ic, PSX_BUS_DEVICE_IC, ##__VA_ARGS__)                    \
    OP(scratchpad, PSX_BUS_DEVICE_SCRATCHPAD, ##__VA_ARGS__)    \
    OP(gpu, PSX_BUS_DEVICE_GPU, ##__VA_ARGS__)                  \
    OP(spu, PSX_BUS_DEVICE_SPU, ##__VA_ARGS__)                  \
    OP(timer, PSX_BUS_DEVICE_TIMER, ##__VA_ARGS__)              \
    OP(cdrom, PSX_BUS_DEVICE_CDROM, ##__VA_ARGS__)              \
    OP(pad, PSX_BUS_DEVICE_PAD, ##__VA_ARGS__)                  \
    OP(mdec, PSX_BUS_DEVICE_MDEC, ##__VA_ARGS__)

static inline void psx_bus_invalidate_cache(psx_bus_t *bus)
{
    if (bus)
    {
        bus->cache.kind = PSX_BUS_DEVICE_NONE;
    }
}

static inline void psx_bus_update_cache(psx_bus_t *bus,
                                        psx_bus_device_kind_t kind,
                                        uint32_t base,
                                        uint32_t size,
                                        uint32_t delay)
{
    bus->cache.kind = kind;
    bus->cache.base = base;
    bus->cache.size = size;
    bus->cache.delay = delay;
}

static inline int psx_bus_cache_ready(const psx_bus_t *bus, uint32_t addr, uint32_t *offset_out)
{
    const psx_bus_cache_t *cache = &bus->cache;

    if (cache->kind == PSX_BUS_DEVICE_NONE)
    {
        return 0;
    }

    const uint32_t delta = addr - cache->base;
    if (delta >= cache->size)
    {
        return 0;
    }

    *offset_out = delta;
    return 1;
}

#define BUS_CACHE_SWITCH_CASE_READ(dev, kind, bits)                              \
    case kind:                                                                  \
        if (bus->dev != NULL)                                                   \
        {                                                                       \
            *out = psx_##dev##_read##bits(bus->dev, offset);                    \
            return 1;                                                           \
        }                                                                       \
        break;

#define BUS_CACHE_SWITCH_CASE_WRITE(dev, kind, bits)                             \
    case kind:                                                                  \
        if (bus->dev != NULL)                                                   \
        {                                                                       \
            psx_##dev##_write##bits(bus->dev, offset, value);                   \
            return 1;                                                           \
        }                                                                       \
        break;

#define BUS_CACHE_DISPATCH_READ(bits)                                            \
    switch (bus->cache.kind)                                                    \
    {                                                                           \
        PSX_BUS_DEVICE_TABLE(BUS_CACHE_SWITCH_CASE_READ, bits)                  \
    case PSX_BUS_DEVICE_NONE:                                                   \
    default:                                                                    \
        break;                                                                  \
    }

#define BUS_CACHE_DISPATCH_WRITE(bits)                                           \
    switch (bus->cache.kind)                                                    \
    {                                                                           \
        PSX_BUS_DEVICE_TABLE(BUS_CACHE_SWITCH_CASE_WRITE, bits)                 \
    case PSX_BUS_DEVICE_NONE:                                                   \
    default:                                                                    \
        break;                                                                  \
    }

const uint32_t g_psx_bus_region_mask_table[] = {
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0x7fffffff, 0x1fffffff, 0xffffffff, 0xffffffff};

// Static buffer for bus instance
static psx_bus_t g_bus_instance;
static int32_t g_bus_instance_used = 0;

psx_bus_t *psx_bus_create(void)
{
    if (g_bus_instance_used)
    {
        return NULL; // Only one instance allowed
    }
    g_bus_instance_used = 1;
    memset(&g_bus_instance, 0, sizeof(g_bus_instance));
    psx_bus_invalidate_cache(&g_bus_instance);
    return &g_bus_instance;
}

void psx_bus_init(psx_bus_t *bus)
{
    if (bus == NULL)
    {
        return;
    }

    bus->access_cycles = 0;
    psx_bus_invalidate_cache(bus);
}

void psx_bus_destroy(psx_bus_t *bus)
{
    if (bus != NULL)
    {
        psx_bus_invalidate_cache(bus);
    }

    // Mark instance as available again
    g_bus_instance_used = 0;
}

void psx_bus_force_reset_singleton(void)
{
    // Force reset singleton state (for testing only)
    g_bus_instance_used = 0;
    psx_bus_invalidate_cache(&g_bus_instance);
}

#define HANDLE_READ(dev, kind, bits)                                             \
    do                                                                           \
    {                                                                            \
        psx_##dev##_t *const device = bus->dev;                                  \
        if (device != NULL)                                                      \
        {                                                                        \
            const uint32_t base = device->io_base;                               \
            const uint32_t offset = addr - base;                                 \
            if (offset < device->io_size)                                        \
            {                                                                    \
                bus->access_cycles = device->bus_delay;                          \
                psx_bus_update_cache(bus, kind, base, device->io_size, device->bus_delay); \
                return psx_##dev##_read##bits(device, offset);                   \
            }                                                                    \
        }                                                                        \
    } while (0)
#define HANDLE_WRITE(dev, kind, bits)                                            \
    do                                                                           \
    {                                                                            \
        psx_##dev##_t *const device = bus->dev;                                  \
        if (device != NULL)                                                      \
        {                                                                        \
            const uint32_t base = device->io_base;                               \
            const uint32_t offset = addr - base;                                 \
            if (offset < device->io_size)                                        \
            {                                                                    \
                bus->access_cycles = device->bus_delay;                          \
                psx_bus_update_cache(bus, kind, base, device->io_size, device->bus_delay); \
                psx_##dev##_write##bits(device, offset, value);                  \
                return;                                                          \
            }                                                                    \
        }                                                                        \
    } while (0)

#define BUS_HANDLE_READ_ENTRY(dev, kind, bits) HANDLE_READ(dev, kind, bits);
#define BUS_HANDLE_WRITE_ENTRY(dev, kind, bits) HANDLE_WRITE(dev, kind, bits);

#define BUS_HANDLE_READ_ALL(bits) PSX_BUS_DEVICE_TABLE(BUS_HANDLE_READ_ENTRY, bits)
#define BUS_HANDLE_WRITE_ALL(bits) PSX_BUS_DEVICE_TABLE(BUS_HANDLE_WRITE_ENTRY, bits)

static inline int psx_bus_try_cached_read32(psx_bus_t *bus, uint32_t addr, uint32_t *out)
{
    uint32_t offset;
    if (!psx_bus_cache_ready(bus, addr, &offset))
    {
        return 0;
    }

    bus->access_cycles = bus->cache.delay;
    BUS_CACHE_DISPATCH_READ(32);

    psx_bus_invalidate_cache(bus);
    return 0;
}

static inline int psx_bus_try_cached_read16(psx_bus_t *bus, uint32_t addr, uint16_t *out)
{
    uint32_t offset;
    if (!psx_bus_cache_ready(bus, addr, &offset))
    {
        return 0;
    }

    bus->access_cycles = bus->cache.delay;
    BUS_CACHE_DISPATCH_READ(16);

    psx_bus_invalidate_cache(bus);
    return 0;
}

static inline int psx_bus_try_cached_read8(psx_bus_t *bus, uint32_t addr, uint8_t *out)
{
    uint32_t offset;
    if (!psx_bus_cache_ready(bus, addr, &offset))
    {
        return 0;
    }

    bus->access_cycles = bus->cache.delay;
    BUS_CACHE_DISPATCH_READ(8);

    psx_bus_invalidate_cache(bus);
    return 0;
}

static inline int psx_bus_try_cached_write32(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    uint32_t offset;
    if (!psx_bus_cache_ready(bus, addr, &offset))
    {
        return 0;
    }

    bus->access_cycles = bus->cache.delay;
    BUS_CACHE_DISPATCH_WRITE(32);

    psx_bus_invalidate_cache(bus);
    return 0;
}

static inline int psx_bus_try_cached_write16(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    uint32_t offset;
    if (!psx_bus_cache_ready(bus, addr, &offset))
    {
        return 0;
    }

    bus->access_cycles = bus->cache.delay;
    BUS_CACHE_DISPATCH_WRITE(16);

    psx_bus_invalidate_cache(bus);
    return 0;
}

static inline int psx_bus_try_cached_write8(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    uint32_t offset;
    if (!psx_bus_cache_ready(bus, addr, &offset))
    {
        return 0;
    }

    bus->access_cycles = bus->cache.delay;
    BUS_CACHE_DISPATCH_WRITE(8);

    psx_bus_invalidate_cache(bus);
    return 0;
}

uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_bus_read32(psx_bus_t *bus, uint32_t addr)
{
    if (bus == NULL)
    {
        return 0x00000000; /* Return 0 if bus is NULL */
    }

    uint32_t vaddr = addr;

    addr &= g_psx_bus_region_mask_table[addr >> 29];

    if (addr & 0x3)
    {
        log_fatal("Unaligned 32-bit read from %08x:%08x", (uint32_t)vaddr, (uint32_t)addr);
    }

    uint32_t cached32;
    if (psx_bus_try_cached_read32(bus, addr, &cached32))
    {
        return cached32;
    }

    BUS_HANDLE_READ_ALL(32);

    /* For embedded environments, avoid slow fprintf operations that can cause hardfaults */
    /* In testing/debugging, you can enable full logging by setting log level appropriately */
    static int32_t unhandled_read_count = 0;
    unhandled_read_count++;

    /* Only log every 100th unhandled read to avoid flooding */
    if (unhandled_read_count % 100 == 1)
    {
        log_warn("Unhandled 32-bit reads detected (count: %d, last addr: %08x:%08x)",
                 unhandled_read_count, (uint32_t)vaddr, (uint32_t)addr);
    }

    return 0x00000000;
}

static uint16_t sio_ctrl;

uint16_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_bus_read16(psx_bus_t *bus, uint32_t addr)
{
    if (bus == NULL)
    {
        return 0x0000; /* Return 0 if bus is NULL */
    }

    bus->access_cycles = 2;

    uint32_t vaddr = addr;

    addr &= g_psx_bus_region_mask_table[addr >> 29];

    if (addr & 0x1)
    {
        log_fatal("Unaligned 16-bit read from %08x:%08x", (uint32_t)vaddr, (uint32_t)addr);
    }

    uint16_t cached16;
    if (psx_bus_try_cached_read16(bus, addr, &cached16))
    {
        return cached16;
    }

    BUS_HANDLE_READ_ALL(16);

    if (addr == 0x1f80105a)
        return sio_ctrl;

    if (addr == 0x1f801054)
        return 0x05;

    if (addr == 0x1f400004)
        return 0xc8;

    if (addr == 0x1f400006)
        return 0x1fe0;

    static uint32_t unhandled_read16_count = 0;
    unhandled_read16_count++;

    /* Only log every 100th unhandled 16-bit read */
    if (unhandled_read16_count % 100 == 1)
    {
        PRINTF("Unhandled 16-bit read from %08x:%08x [count: %u]\r\n",
               (uint32_t)vaddr, (uint32_t)addr, (uint32_t)unhandled_read16_count);
    }

    // exit(1);

    return 0x0000;
}

uint8_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_bus_read8(psx_bus_t *bus, uint32_t addr)
{
    if (bus == NULL)
    {
        return 0x00; /* Return 0 if bus is NULL */
    }

    bus->access_cycles = 2;

    // uint32_t vaddr = addr;

    addr &= g_psx_bus_region_mask_table[addr >> 29];

    uint8_t cached8;
    if (psx_bus_try_cached_read8(bus, addr, &cached8))
    {
        return cached8;
    }

    BUS_HANDLE_READ_ALL(8);

    // PRINTF("Unhandled 8-bit read from %08lx:%08lx\n", vaddr, addr);

    // exit(1);

    return 0x00;
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_bus_write32(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    if (bus == NULL)
    {
        return; /* Do nothing if bus is NULL */
    }

    bus->access_cycles = 0;

    uint32_t vaddr = addr;

    addr &= g_psx_bus_region_mask_table[addr >> 29];

    if (addr & 0x3)
    {
        log_fatal("Unaligned 32-bit write to %08x:%08x (%08x)", (uint32_t)vaddr, (uint32_t)addr, (uint32_t)value);
    }

    if (psx_bus_try_cached_write32(bus, addr, value))
    {
        return;
    }

    BUS_HANDLE_WRITE_ALL(32);

    static uint32_t unhandled_write_count = 0;
    unhandled_write_count++;
    if (unhandled_write_count % 100 == 1)
    { /* Print every 100th unhandled write */
        PRINTF("Unhandled 32-bit write to %08x:%08x (%08x) [count: %u]\r\n",
               (uint32_t)vaddr, (uint32_t)addr, (uint32_t)value,
               (uint32_t)unhandled_write_count);
    }

    // exit(1);
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_bus_write16(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    if (bus == NULL)
    {
        return; /* Do nothing if bus is NULL */
    }

    bus->access_cycles = 0;

    uint32_t vaddr = addr;

    addr &= g_psx_bus_region_mask_table[addr >> 29];

    if (addr & 0x1)
    {
        log_fatal("Unaligned 16-bit write to %08x:%08x (%04x)", (uint32_t)vaddr, (uint32_t)addr, value);
    }

    if (psx_bus_try_cached_write16(bus, addr, value))
    {
        return;
    }

    BUS_HANDLE_WRITE_ALL(16);

    // if (addr == 0x1f80105a) { sio_ctrl = value; return; }

    PRINTF("Unhandled 16-bit write to %08x:%08x (%04x)\r\n", (uint32_t)vaddr, (uint32_t)addr, (uint32_t)value);

    // exit(1);
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_bus_write8(psx_bus_t *bus, uint32_t addr, uint32_t value)
{
    if (bus == NULL)
    {
        return; /* Do nothing if bus is NULL */
    }

    bus->access_cycles = 0;

    uint32_t vaddr = addr;

    addr &= g_psx_bus_region_mask_table[addr >> 29];

    if (psx_bus_try_cached_write8(bus, addr, value))
    {
        return;
    }

    BUS_HANDLE_WRITE_ALL(8);

    PRINTF("Unhandled 8-bit write to %08x:%08x (%02x)\r\n", (uint32_t)vaddr, (uint32_t)addr, (uint32_t)value);

    // exit(1);
}

#define DEFINE_BUS_INIT_FN(dev, kind, ...)                                      \
    void psx_bus_init_##dev(psx_bus_t *bus, psx_##dev##_t *value)               \
    {                                                                           \
        bus->dev = value;                                                       \
        psx_bus_invalidate_cache(bus);                                          \
    }

PSX_BUS_DEVICE_TABLE(DEFINE_BUS_INIT_FN);
#undef DEFINE_BUS_INIT_FN

uint32_t psx_bus_get_access_cycles(psx_bus_t *bus)
{
    if (bus == NULL)
    {
        return 0; /* Return 0 cycles if bus is NULL */
    }

    uint32_t cycles = bus->access_cycles;

    bus->access_cycles = 0;

    return cycles;
}

#undef HANDLE_READ
#undef HANDLE_WRITE
