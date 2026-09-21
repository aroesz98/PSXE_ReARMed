/*
    Recompiler core: code tiers, block lookup, translation and dispatch.

    Every block is translated once into a large code area in SDRAM, which holds
    the working set of a whole game scene, so nothing is ever translated twice
    just because the fast memory ran out. The 80 KB of ITCM are a cache on top
    of that: the blocks that actually run the most are copied into it, chosen
    from execution counts, and the choice is revised as the hot code moves.

    Blocks refer to one another through links (see jit_translate.h), so a block
    can be copied, moved between the tiers or invalidated by updating one
    pointer, and a block that ends in a branch with a known target jumps
    straight into the next block instead of returning here.
*/

#include <stdint.h>
#include <string.h>

#include "jit.h"
#include "jit_emit.h"
#include "jit_translate.h"
#include "jit_block.h"

#include "../bus.h"
#include "../bus_init.h"
#include "../bus_fast.h"
#include "../prof.h"
#include "fsl_debug_console.h"

#if PSX_JIT_ENABLE

/* ------------------------------------------------------------------ memory */

/* Hot tier. Generated code runs from ITCM with zero wait states and without
   touching the instruction cache. */
static uint8_t __attribute__((section(".bss.$SRAM_ITC"), aligned(8))) g_jit_code[PSX_JIT_CODE_SIZE];

/* Every block lives here; this copy is what the hot tier is filled from. Code
   that runs from here goes through the instruction cache, so what costs is a
   cache line fill now and then - a small fraction of translating it again. */
static uint8_t __attribute__((section(".bss.$BOARD_SDRAM"), aligned(32))) g_jit_master[PSX_JIT_MASTER_SIZE];

/*
    The bookkeeping, split by how often it is touched. Blocks are referred to by
    index + 1 so that 0 can mean "none".

      g_jit_link   the entry points, read on every jump from one block into the
                   next: DTCM, one cycle, and nothing of the data cache spent
      g_jit_pc     the guest address of each, read by the lookup on every
      g_jit_meta   dispatch, and what else a dispatch touches: OCRAM
      g_jit_cold   what only translating, invalidating and revising the tiers
                   look at: SDRAM
*/
static psx_jit_link_t __attribute__((section(".bss.$SRAM_DTC"))) g_jit_link[PSX_JIT_MAX_BLOCKS];
static uint32_t __attribute__((section(".bss.$SRAM_OC"))) g_jit_pc[PSX_JIT_MAX_BLOCKS];

typedef struct
{
    uint16_t next; /* hash chain                                          */
    uint16_t heat; /* dispatches since the tiers were last revised        */
    uint8_t hot;   /* runs from the ITCM copy                             */
    uint8_t instr; /* guest instructions covered; 0: a link only, no code */
} psx_jit_meta_t;

typedef struct
{
    uint32_t master;    /* offset of the code inside g_jit_master */
    uint16_t size;      /* bytes of code                          */
    uint16_t page_next; /* blocks built from one guest page       */
} psx_jit_cold_t;

static psx_jit_meta_t __attribute__((section(".bss.$SRAM_OC"))) g_jit_meta[PSX_JIT_MAX_BLOCKS];
static psx_jit_cold_t __attribute__((section(".bss.$BOARD_SDRAM"))) g_jit_cold[PSX_JIT_MAX_BLOCKS];
static uint16_t __attribute__((section(".bss.$SRAM_OC"))) g_jit_hash[PSX_JIT_HASH_SIZE];

/* 1 KB page granularity over the 2 MB of guest RAM. Coarser pages caused code
   and data that merely live close to each other to invalidate one another. */
#define PSX_JIT_PAGE_SHIFT 10
#define PSX_JIT_PAGE_COUNT (0x200000u >> PSX_JIT_PAGE_SHIFT)
#define PSX_JIT_PAGE_MASK ((1u << PSX_JIT_PAGE_SHIFT) - 1u)

static uint16_t __attribute__((section(".bss.$SRAM_OC"))) g_jit_page_head[PSX_JIT_PAGE_COUNT];

/*
    Where inside a page the translated code is: one bit per 32 bytes.

    Games keep data right next to code - FF7's battle module writes variables
    that share a 1 KB page with its inner loops several hundred times a second -
    and dropping every block of the page for each of those writes meant
    translating the same blocks again a thousand times a second, running them
    cold from SDRAM in between, and breaking the links into them. A store is
    now checked against this map first, and only a store that lands in a cell
    that holds code walks the page's blocks - and takes away the ones it really
    overwrites.
*/
static uint32_t __attribute__((section(".bss.$SRAM_OC"))) g_jit_page_cells[PSX_JIT_PAGE_COUNT];

/* The guest bytes a block depends on: its instructions and the one behind them,
   which the translator looks at to decide how the last load is delivered. */
static inline uint32_t jit_block_span(uint32_t index, uint32_t *lo)
{
    *lo = g_jit_pc[index - 1u] & 0x1fffffu;

    return ((uint32_t)g_jit_meta[index - 1u].instr + 1u) * 4u;
}

/* bits of the 32 byte cells that [lo, lo + len) touches inside its page */
static inline uint32_t jit_cell_mask(uint32_t lo, uint32_t len)
{
    const uint32_t first = (lo & PSX_JIT_PAGE_MASK) >> 5;

    uint32_t last = ((lo & PSX_JIT_PAGE_MASK) + (len ? (len - 1u) : 0u)) >> 5;

    if (last > 31u)
        last = 31u;

    return (0xffffffffu >> (31u - last)) & (0xffffffffu << first);
}

/* Shared with the memory write fast path (see bus_fast.h) and with every store
   a block does: one byte per 256 bytes of guest RAM, so this one stays in DTCM.
   It is the cell map above, eight cells to the byte. */
#define PSX_JIT_FLAGS_PER_PAGE (1u << (PSX_JIT_PAGE_SHIFT - PSX_JIT_CODE_FLAG_SHIFT))

uint8_t __attribute__((section(".bss.$SRAM_DTC"))) g_psx_jit_code_pages[PSX_JIT_PAGE_COUNT * PSX_JIT_FLAGS_PER_PAGE];

_Static_assert((PSX_JIT_PAGE_SHIFT == 10) && (PSX_JIT_CODE_FLAG_SHIFT == 8u), "four flags of eight cells to the page");

static inline void jit_page_set_cells(uint32_t page, uint32_t cells)
{
    g_jit_page_cells[page] = cells;

    uint8_t *const f = &g_psx_jit_code_pages[page * PSX_JIT_FLAGS_PER_PAGE];

    f[0] = (cells & 0x000000ffu) ? 1u : 0u;
    f[1] = (cells & 0x0000ff00u) ? 1u : 0u;
    f[2] = (cells & 0x00ff0000u) ? 1u : 0u;
    f[3] = (cells & 0xff000000u) ? 1u : 0u;
}

/* A block is translated into this much scratch space and copied out once its
   size is known. The longest translation of one instruction is below 100 bytes. */
#define PSX_JIT_SCRATCH_BYTES 2560u

/* What the entry stub loads the block registers from; the helper table r9
   points at is the tail of it. */
typedef struct
{
    uint32_t ram;   /* r6  */
    uint32_t pages; /* r7  */
    uint32_t exit;  /* r10 */
    uint32_t helpers[PSX_JIT_H_COUNT];
} psx_jit_regs_t;

static psx_jit_regs_t __attribute__((section(".bss.$SRAM_DTC"))) g_jit_regs;

static uint32_t g_jit_block_count;
static uint32_t g_jit_master_used;
static uint32_t g_jit_hot_used;
static psx_jit_stats_t g_jit_stats;

/* Dispatches that ran a block from SDRAM since the tiers were last revised,
   and how many of those it takes to revise them again. */
#ifndef PSX_JIT_RETIER_BASE
#define PSX_JIT_RETIER_BASE 16384u
#endif
#define PSX_JIT_RETIER_MAX_SHIFT 6u

static uint32_t g_jit_cold_runs;
static uint32_t g_jit_retier_at = PSX_JIT_RETIER_BASE;
static uint32_t g_jit_retier_shift;

/* Emulated cycles accumulated by the fallback inside the running blocks */
static uint32_t g_jit_cycles;

/* ------------------------------------------------------------------ entry stub */

/*
    Runs translated code: sets up the registers every block relies on and jumps
    to `code`. Blocks hand over to one another without coming back; whichever
    block stops - out of budget, a register jump, a miss - returns to
    psx_jit_block_exit through r10, and r5 is the emulated cycles of all of them.

    psx_jit_exit_link is the way out for a block that would have jumped into
    another one but may not (the budget is used up) or cannot (the link has no
    code): r3 is that link, and since a jump from block to block publishes
    nothing, the guest pc the link stands for is stored here, on the way out.
*/
uint32_t psx_jit_enter(psx_cpu_t *cpu, uint32_t code, uint32_t budget, const psx_jit_regs_t *regs);
void psx_jit_block_exit(void);
void psx_jit_exit_link(void);

_Static_assert((offsetof(psx_cpu_t, pc) == 132u) && (offsetof(psx_cpu_t, next_pc) == 136u),
               "psx_jit_exit_link stores pc / next_pc at fixed offsets");
_Static_assert(PSX_JIT_H_PC_DELTA == 40u, "psx_jit_exit_link reads the delta from [r9, #40]");

__attribute__((naked, noinline, used, section(".ramfunc.$SRAM_ITC")))
uint32_t psx_jit_enter(psx_cpu_t *cpu, uint32_t code, uint32_t budget, const psx_jit_regs_t *regs)
{
    __asm__(
        "push {r4-r10, lr}\n"
        "mov r4, r0\n"
        "movs r5, #0\n"
        "mov r8, r2\n"
        "ldr r6, [r3, #0]\n"
        "ldr r7, [r3, #4]\n"
        "ldr r10, [r3, #8]\n"
        "add r9, r3, #12\n"
        "bx r1\n"
        ".balign 4\n"
        ".global psx_jit_exit_link\n"
        ".thumb_func\n"
        ".type psx_jit_exit_link, %function\n"
        "psx_jit_exit_link:\n"
        "ldr r0, [r9, #40]\n"
        "ldr r0, [r3, r0]\n"
        "adds r1, r0, #4\n"
        "strd r0, r1, [r4, #132]\n"
        ".global psx_jit_block_exit\n"
        ".thumb_func\n"
        ".type psx_jit_block_exit, %function\n"
        "psx_jit_block_exit:\n"
        "mov r0, r5\n"
        "pop {r4-r10, pc}\n"
    );
}

/* ------------------------------------------------------------------ helpers */

#if PSX_JIT_HIST
static uint32_t __attribute__((section(".bss.$SRAM_DTC"))) g_jit_hist[PSX_JIT_HIST_SIZE];

const uint32_t *psx_jit_get_hist(void)
{
    return g_jit_hist;
}

void psx_jit_clear_hist(void)
{
    for (uint32_t i = 0; i < PSX_JIT_HIST_SIZE; i++)
        g_jit_hist[i] = 0;
}

static inline void jit_hist_note(uint32_t opcode)
{
    const uint32_t o = opcode >> 26;

    g_jit_hist[o ? o : (64u + (opcode & 0x3fu))]++;
}
#endif

/*
    What an interpreted instruction in the middle of a block reports back: non
    zero makes the block return to the dispatcher.

    That is the case when control flow left the straight line (cpu->saved_pc is
    the address the interpreter fetched from, so the block does not have to pass
    the expected pc in) - and when the instruction produced state that only the
    dispatcher deals with: an interrupt that is now pending and enabled, or an
    isolated cache. Blocks hand over to each other without looking at either, so
    this is the one place that keeps both from going unnoticed until the end of
    the slice; pc / next_pc are correct here, the interpreter just ran.
*/
static inline uint32_t jit_after_interp(psx_cpu_t *cpu)
{
    g_jit_cycles += cpu->last_cycles;
    g_jit_stats.interp_steps++;

#if PSX_JIT_HIST
    jit_hist_note(cpu->opcode);
#endif

    if (cpu->pc != (cpu->saved_pc + 4u))
        return 1u;

    const uint32_t sr = cpu->cop0_r[COP0_SR];

    if ((sr & SR_ISC) || ((sr & SR_IEC) && (sr & cpu->cop0_r[COP0_CAUSE] & 0x00000700u)))
        return 1u;

    /* A device register was written: the slice this block runs in was sized
       from the devices' deadlines, and one of them may just have moved closer.
       psx_update looks at it again before anything else runs. */
    if (g_psx_bus_io_written)
        return 1u;

    return 0u;
}

/* One instruction through the interpreter; pc / next_pc are already right */
uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_interp_op(psx_cpu_t *cpu)
{
    psx_cpu_cycle(cpu);

    return jit_after_interp(cpu);
}

/* The same after natively translated instructions, which do not advance the pc */
uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_interp_op_at(psx_cpu_t *cpu, uint32_t pc)
{
    cpu->pc = pc;
    cpu->next_pc = pc + 4u;

    psx_cpu_cycle(cpu);

    return jit_after_interp(cpu);
}

/* Escape path of a natively translated load: the address turned out not to be
   plain RAM, so the interpreter runs the load - which leaves the value in the
   load delay slot. The instruction that follows is native code and would never
   apply it, so it is applied here. That is equivalent: the translator only
   takes the native path when the following instruction neither reads nor writes
   the loaded register, so nobody can observe the earlier write back. */
uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_interp_load_at(psx_cpu_t *cpu, uint32_t pc)
{
    cpu->pc = pc;
    cpu->next_pc = pc + 4u;

    psx_cpu_cycle(cpu);

    if (cpu->pc != (cpu->saved_pc + 4u))
    {
        /* exception: the load stays pending, the block exits */
        g_jit_cycles += cpu->last_cycles;
        g_jit_stats.interp_steps++;

        return 1u;
    }

    cpu->r[cpu->load_d] = cpu->load_v;
    cpu->r[0] = 0;
    cpu->load_v = 0xffffffffu;
    cpu->load_d = 0;

    return jit_after_interp(cpu);
}

/*
    Escape of a natively translated load or store whose address is not plain RAM.

    Nearly all of those are the scratchpad - FF7 keeps its 3D working set there -
    and that is memory like any other, so it is served here, from what the block
    passes along about the access, at a fraction of an interpreter step (whose
    fetch alone is a data cache miss). Anything else, and anything misaligned,
    still goes to the interpreter, which does the device access or raises the
    address error.
*/
uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_mem_escape(psx_cpu_t *cpu, uint32_t pc,
                                                                           uint32_t addr, uint32_t desc)
{
    const uint32_t off = (addr & 0x1fffffffu) - PSX_SPAD_FAST_BASE;
    const uint32_t size = PSX_JIT_MD_SIZE(desc);

    if ((off < PSX_SPAD_FAST_SIZE) && !(addr & (size - 1u)))
    {
        uint8_t *const p = cpu->bus->scratchpad->buf + off;
        const uint32_t rt = PSX_JIT_MD_RT(desc);

        if (desc & PSX_JIT_MD_STORE)
        {
            const uint32_t v = cpu->r[rt];

            if (size == 4u)
                *(uint32_t *)p = v;
            else if (size == 2u)
                *(uint16_t *)p = (uint16_t)v;
            else
                *p = (uint8_t)v;
        }
        else
        {
            uint32_t v;

            if (size == 4u)
                v = *(const uint32_t *)p;
            else if (size == 2u)
                v = (desc & PSX_JIT_MD_SIGNED) ? (uint32_t)(int32_t)*(const int16_t *)p : *(const uint16_t *)p;
            else
                v = (desc & PSX_JIT_MD_SIGNED) ? (uint32_t)(int32_t)*(const int8_t *)p : *p;

            if (desc & PSX_JIT_MD_PENDING)
            {
                /* as the interpreter leaves it: the next instruction, which
                   the block has interpreted, applies it */
                cpu->load_d = rt;
                cpu->load_v = v;
            }
            else if (rt)
            {
                cpu->r[rt] = v;
            }
        }

        const uint32_t cycles = 2u + cpu->bus->scratchpad->bus_delay;

        g_jit_cycles += cycles;
        cpu->total_cycles += cycles;

        return 0u;
    }

    /* A store into RAM only gets here because its page holds translated code.
       Most of those hit the data that lives next to the code: written right
       here, and whatever code it does overwrite is dropped. (With the cache
       isolated the dispatcher interprets everything, so that never gets here.) */
    if ((desc & PSX_JIT_MD_STORE) && !(addr & 0x1fe00000u) && !(addr & (size - 1u)))
    {
        const uint32_t phys = addr & 0x1fffffu;
        uint8_t *const p = cpu->bus->ram->buf + phys;
        const uint32_t v = cpu->r[PSX_JIT_MD_RT(desc)];

        if (size == 4u)
            *(uint32_t *)p = v;
        else if (size == 2u)
            *(uint16_t *)p = (uint16_t)v;
        else
            *p = (uint8_t)v;

        if (g_jit_page_cells[phys >> PSX_JIT_PAGE_SHIFT] & jit_cell_mask(phys, size))
            psx_jit_invalidate(phys, size);

        g_jit_cycles += 2u;
        cpu->total_cycles += 2u;

        return 0u;
    }

    if ((desc & PSX_JIT_MD_STORE) || (desc & PSX_JIT_MD_PENDING))
        return psx_jit_interp_op_at(cpu, pc);

    return psx_jit_interp_load_at(cpu, pc);
}

/* LWC2 / SWC2 */
uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_gte_transfer(psx_cpu_t *cpu, uint32_t pc,
                                                                             uint32_t opcode)
{
    const uint32_t diverged = psx_cpu_gte_transfer(cpu, pc, opcode);

    g_jit_cycles += cpu->last_cycles;

    if (diverged)
        return 1u;

    /* the store may have been to a device register */
    return g_psx_bus_io_written ? 1u : 0u;
}

/* A delay slot the block could not run natively after all: the interpreter runs
   it with the branch pending, and leaves pc at the target (or at an exception
   vector). The block ends right after, so there is nothing to report. */
void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_interp_delay(psx_cpu_t *cpu, uint32_t pc,
                                                                         uint32_t next_pc)
{
    cpu->pc = pc;
    cpu->next_pc = next_pc;
    cpu->branch = 1;

    psx_cpu_cycle(cpu);

    g_jit_cycles += cpu->last_cycles;
    g_jit_stats.interp_steps++;

#if PSX_JIT_HIST
    jit_hist_note(cpu->opcode);
#endif
}

/* ------------------------------------------------------------------ lookup */

static inline uint32_t jit_hash(uint32_t pc)
{
    return (pc >> 2) & (PSX_JIT_HASH_SIZE - 1u);
}

/* index + 1 of the block at pc, 0 when there is none */
static inline uint32_t jit_lookup(uint32_t pc)
{
    uint32_t i = g_jit_hash[jit_hash(pc)];

    while (i)
    {
        if (g_jit_pc[i - 1u] == pc)
            return i;

        i = g_jit_meta[i - 1u].next;
    }

    return 0;
}

/* The block at pc, created as a bare link when it is not known yet */
static uint32_t jit_get_block(uint32_t pc)
{
    uint32_t i = jit_lookup(pc);

    if (i)
        return i;

    if (g_jit_block_count >= PSX_JIT_MAX_BLOCKS)
        return 0;

    i = ++g_jit_block_count;

    psx_jit_meta_t *const m = &g_jit_meta[i - 1u];

    g_jit_pc[i - 1u] = pc;
    g_jit_link[i - 1u].code = g_jit_regs.helpers[PSX_JIT_H_EXIT_LINK / 4u];

    m->heat = 0;
    m->hot = 0;
    m->instr = 0;
    m->next = g_jit_hash[jit_hash(pc)];

    g_jit_cold[i - 1u].master = 0;
    g_jit_cold[i - 1u].size = 0;
    g_jit_cold[i - 1u].page_next = 0;

    g_jit_hash[jit_hash(pc)] = (uint16_t)i;
    g_jit_stats.blocks = g_jit_block_count;

    return i;
}

void psx_jit_reset(void)
{
    memset(g_jit_hash, 0, sizeof(g_jit_hash));
    memset(g_jit_page_head, 0, sizeof(g_jit_page_head));
    memset(g_jit_page_cells, 0, sizeof(g_jit_page_cells));
    memset(g_psx_jit_code_pages, 0, sizeof(g_psx_jit_code_pages));

    g_jit_block_count = 0;
    g_jit_master_used = 0;
    g_jit_hot_used = 0;

    g_jit_cold_runs = 0;
    g_jit_retier_shift = 0;
    g_jit_retier_at = PSX_JIT_RETIER_BASE;

    g_jit_stats.blocks = 0;
    g_jit_stats.code_used = 0;
    g_jit_stats.hot_used = 0;
    g_jit_stats.hot_blocks = 0;
    g_jit_stats.flushes++;
}

void psx_jit_init(void)
{
    memset(&g_jit_stats, 0, sizeof(g_jit_stats));
    memset(&g_jit_regs, 0, sizeof(g_jit_regs));

    g_jit_regs.pages = (uint32_t)(uintptr_t)g_psx_jit_code_pages;
    g_jit_regs.exit = (uint32_t)(uintptr_t)&psx_jit_block_exit | 1u;

    g_jit_regs.helpers[PSX_JIT_H_INTERP / 4u] = (uint32_t)(uintptr_t)&psx_jit_interp_op;
    g_jit_regs.helpers[PSX_JIT_H_INTERP_AT / 4u] = (uint32_t)(uintptr_t)&psx_jit_interp_op_at;
    g_jit_regs.helpers[PSX_JIT_H_LOAD_AT / 4u] = (uint32_t)(uintptr_t)&psx_jit_interp_load_at;
    g_jit_regs.helpers[PSX_JIT_H_DELAY / 4u] = (uint32_t)(uintptr_t)&psx_jit_interp_delay;
    g_jit_regs.helpers[PSX_JIT_H_GTE / 4u] = (uint32_t)(uintptr_t)&psx_cpu_gte_command;
    g_jit_regs.helpers[PSX_JIT_H_GTE_READ / 4u] = (uint32_t)(uintptr_t)&psx_cpu_gte_read;
    g_jit_regs.helpers[PSX_JIT_H_GTE_WRITE / 4u] = (uint32_t)(uintptr_t)&psx_cpu_gte_write;
    g_jit_regs.helpers[PSX_JIT_H_GTE_MEM / 4u] = (uint32_t)(uintptr_t)&psx_jit_gte_transfer;
    g_jit_regs.helpers[PSX_JIT_H_MEM / 4u] = (uint32_t)(uintptr_t)&psx_jit_mem_escape;
    g_jit_regs.helpers[PSX_JIT_H_EXIT_LINK / 4u] = (uint32_t)(uintptr_t)&psx_jit_exit_link | 1u;
    g_jit_regs.helpers[PSX_JIT_H_PC_DELTA / 4u] = (uint32_t)(uintptr_t)g_jit_pc - (uint32_t)(uintptr_t)g_jit_link;

    psx_jit_reset();

    g_jit_stats.flushes = 0;
}

/* Takes the code away from one block. Its link stays - other blocks jump
   through it - and points back at the dispatcher, which translates the block
   again the next time it is reached. The code it occupied is not reclaimed; the
   code area is only ever emptied as a whole. */
static void jit_kill_block(uint32_t i)
{
    psx_jit_meta_t *const m = &g_jit_meta[i - 1u];

    g_jit_link[i - 1u].code = g_jit_regs.helpers[PSX_JIT_H_EXIT_LINK / 4u];

    if (m->instr)
        g_jit_stats.killed++;

    m->instr = 0;
    m->hot = 0;
    m->heat = 0;

    g_jit_cold[i - 1u].size = 0;
    g_jit_cold[i - 1u].page_next = 0;
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_invalidate(uint32_t addr, uint32_t size)
{
    if ((addr & 0x1fffffffu) >= 0x200000u)
        return; /* not guest RAM: nothing can have been compiled from it */

    const uint32_t lo = addr & 0x1fffffu;
    const uint32_t hi = lo + (size ? size : 1u); /* exclusive */
    const uint32_t first = lo >> PSX_JIT_PAGE_SHIFT;
    const uint32_t last = (hi - 1u) >> PSX_JIT_PAGE_SHIFT;

    for (uint32_t p = first; (p <= last) && (p < PSX_JIT_PAGE_COUNT); p++)
    {
        if (!g_jit_page_head[p])
            continue;

        /* the part of the write that falls into this page, against the map */
        const uint32_t page_lo = p << PSX_JIT_PAGE_SHIFT;
        const uint32_t w_lo = (lo > page_lo) ? lo : page_lo;
        const uint32_t w_hi = (hi < (page_lo + PSX_JIT_PAGE_MASK + 1u)) ? hi : (page_lo + PSX_JIT_PAGE_MASK + 1u);

        if (!(g_jit_page_cells[p] & jit_cell_mask(w_lo, w_hi - w_lo)))
            continue; /* data that shares the page with code */

        uint16_t *link_to_me = &g_jit_page_head[p];
        uint32_t i = *link_to_me;
        uint32_t cells = 0;
        uint32_t killed = 0;

        while (i)
        {
            psx_jit_cold_t *const m = &g_jit_cold[i - 1u];
            const uint32_t next = m->page_next;

            uint32_t b_lo;
            const uint32_t b_len = jit_block_span(i, &b_lo);

            if ((b_lo < hi) && ((b_lo + b_len) > lo))
            {
                *link_to_me = (uint16_t)next;

                jit_kill_block(i);

                killed++;
            }
            else
            {
                cells |= jit_cell_mask(b_lo, b_len);
                link_to_me = &m->page_next;
            }

            i = next;
        }

        if (killed)
            g_jit_stats.invalidations++;

        /* what is left of the page */
        jit_page_set_cells(p, cells);
    }
}

/* ------------------------------------------------------------------ tiers */

/* 0 for a block that never ran, otherwise the position of the top bit + 1 */
static inline uint32_t jit_heat_class(uint32_t heat)
{
    return heat ? (32u - (uint32_t)__builtin_clz(heat)) : 0u;
}

/*
    Revises which blocks run from ITCM.

    The blocks are ranked by how often the dispatcher entered them since the last
    revision (halved each time, so the past fades rather than vanishes) and the
    hot tier is filled from the top: whole heat classes while they fit, then as
    much of the next one as there is room for. The tier is rebuilt from the
    master copies rather than patched up, which keeps it free of holes; that is
    80 KB of copying at worst, and it only happens when enough dispatches ran
    from SDRAM to be worth it.

    Only called from the dispatcher, never while a block is on the stack.
*/
static void jit_retier(void)
{
    PROF_T0(t_tier);

    uint32_t bytes[18];

    for (uint32_t k = 0; k < 18u; k++)
        bytes[k] = 0;

    for (uint32_t i = 0; i < g_jit_block_count; i++)
    {
        const psx_jit_meta_t *const m = &g_jit_meta[i];

        if (m->instr)
            bytes[jit_heat_class(m->heat)] += g_jit_cold[i].size;
    }

    /* classes `full` and above go in entirely, class `part` as far as it fits */
    uint32_t room = PSX_JIT_CODE_SIZE;
    uint32_t full = 17u;
    uint32_t part = 0;

    for (uint32_t k = 16u; k >= 1u; k--)
    {
        if (bytes[k] > room)
        {
            part = k;
            break;
        }

        room -= bytes[k];
        full = k;
    }

    uint32_t used = 0;
    uint32_t hot_blocks = 0;
    uint32_t promoted = 0;

    for (uint32_t i = 0; i < g_jit_block_count; i++)
    {
        psx_jit_meta_t *const m = &g_jit_meta[i];

        if (!m->instr)
            continue;

        const uint32_t size = g_jit_cold[i].size;
        const uint32_t master = g_jit_cold[i].master;
        const uint32_t k = jit_heat_class(m->heat);

        int want = (k >= full);

        if (!want && part && (k == part) && (size <= room))
        {
            want = 1;
            room -= size;
        }

        if (want && ((used + size) <= PSX_JIT_CODE_SIZE))
        {
            memcpy(&g_jit_code[used], &g_jit_master[master], size);

            g_jit_link[i].code = (uint32_t)(uintptr_t)&g_jit_code[used] | 1u;

            used += size;
            hot_blocks++;

            if (!m->hot)
                promoted++;

            m->hot = 1;
        }
        else
        {
            g_jit_link[i].code = (uint32_t)(uintptr_t)&g_jit_master[master] | 1u;

            m->hot = 0;
        }

        m->heat >>= 1;
    }

    g_jit_hot_used = used;

    /* ITCM was written through the data side: make it visible to fetch */
    __asm volatile("dsb\n\tisb" ::: "memory");

    /* When a revision changes next to nothing the hot code simply does not fit,
       and revising it again soon would only burn time: back off. */
    if (promoted < 8u)
    {
        if (g_jit_retier_shift < PSX_JIT_RETIER_MAX_SHIFT)
            g_jit_retier_shift++;
    }
    else
    {
        g_jit_retier_shift = 0;
    }

    g_jit_retier_at = PSX_JIT_RETIER_BASE << g_jit_retier_shift;
    g_jit_cold_runs = 0;

    g_jit_stats.retiers++;
    g_jit_stats.hot_used = used;
    g_jit_stats.hot_blocks = hot_blocks;

    PROF_ADD(jit_tier, t_tier);
}

/* ------------------------------------------------------------------ compile */

static uint32_t jit_read32(void *ud, uint32_t addr)
{
    return psx_bus_fast_read32(((psx_cpu_t *)ud)->bus, addr);
}

static uint32_t jit_link_of(void *ud, uint32_t pc)
{
    (void)ud;

    const uint32_t i = jit_get_block(pc);

    return i ? (uint32_t)(uintptr_t)&g_jit_link[i - 1u] : 0u;
}

/* Translates the block at pc; returns its index + 1, 0 when that failed */
static uint32_t jit_compile(psx_cpu_t *cpu, uint32_t pc)
{
    /* Code is only taken from RAM and the BIOS. Anything else can change
       without psx_jit_invalidate() hearing of it, which would leave a stale
       block behind - and a pc out there is a program that has crashed anyway,
       so it is left to the interpreter. (Found by the differential test: a
       random program that jumped into the I/O window.) */
    {
        const uint32_t phys = pc & 0x1fffffffu;

        if ((phys >= 0x00200000u) && ((phys < 0x1fc00000u) || (phys >= 0x1fc80000u)))
            return 0;
    }

    PROF_T0(t_cmp);

    /* A block needs a link for itself and one for each way out of it, and the
       code area is only ever emptied as a whole: no block is running while the
       dispatcher is here, so nothing can be left pointing into it. */
    if (((g_jit_block_count + 3u) > PSX_JIT_MAX_BLOCKS) ||
        ((PSX_JIT_MASTER_SIZE - g_jit_master_used) < PSX_JIT_SCRATCH_BYTES))
    {
        psx_jit_reset();
    }

    g_jit_regs.ram = (uint32_t)(uintptr_t)cpu->bus->ram->buf;

    const uint32_t index = jit_get_block(pc);

    if (!index)
        return 0;

    uint16_t scratch[PSX_JIT_SCRATCH_BYTES / 2u];

    psx_emit_t e;
    psx_jit_ctx_t ctx;
    psx_jit_block_info_t info;

    int ok = 0;

    /* A block that does not fit the scratch space is cut shorter; with the
       sizes above that is a formality. */
    for (uint32_t max_instr = PSX_JIT_MAX_INSTR; max_instr && !ok; max_instr >>= 1)
    {
        psx_emit_init(&e, scratch, (uint32_t)sizeof(scratch));

        ctx.e = &e;
        ctx.read32 = jit_read32;
        ctx.get_link = jit_link_of;
        ctx.link_pc_delta = g_jit_regs.helpers[PSX_JIT_H_PC_DELTA / 4u];
        ctx.ud = cpu;

        ok = psx_jit_build_block(&ctx, pc, max_instr, PSX_JIT_PAGE_MASK, &info);
    }

    if (!ok)
    {
        PROF_ADD(jit_cmp, t_cmp);
        return 0;
    }

    const uint32_t size = (psx_emit_size(&e) + 3u) & ~3u;

    uint8_t *const code = &g_jit_master[g_jit_master_used];

    memcpy(code, scratch, size);

    /* The code went in through the data cache and will be fetched through the
       instruction cache: push it out to SDRAM and drop whatever the instruction
       cache still holds for these addresses from before the last flush. */
    SCB_CleanDCache_by_Addr((void *)code, (int32_t)size);
    SCB_InvalidateICache_by_Addr((void *)code, (int32_t)size);

    psx_jit_meta_t *const m = &g_jit_meta[index - 1u];
    psx_jit_cold_t *const cold = &g_jit_cold[index - 1u];

    cold->master = g_jit_master_used;
    cold->size = (uint16_t)size;

    m->instr = (uint8_t)info.instr;
    m->heat = 0;
    m->hot = 0;

    g_jit_link[index - 1u].code = (uint32_t)(uintptr_t)code | 1u; /* Thumb */

    g_jit_master_used += size;

    g_jit_stats.code_used = g_jit_master_used;
    g_jit_stats.compiles++;
    g_jit_stats.native += info.native;

    /* Register the block with the page it was translated from, so a store into
       that page only drops the blocks actually built from it. A block never
       spans a page: translation stops at the page boundary. */
    const uint32_t phys = pc & 0x1fffffffu;

    if (phys < 0x200000u)
    {
        const uint32_t page = phys >> PSX_JIT_PAGE_SHIFT;

        cold->page_next = g_jit_page_head[page];

        g_jit_page_head[page] = (uint16_t)index;

        uint32_t b_lo;
        const uint32_t b_len = jit_block_span(index, &b_lo);

        jit_page_set_cells(page, g_jit_page_cells[page] | jit_cell_mask(b_lo, b_len));
    }

    PROF_ADD(jit_cmp, t_cmp);

    return index;
}

/* ------------------------------------------------------------------ trap */

/* Diagnostic: the guest pc must always point at RAM, BIOS or the scratchpad.
   When a translation bug corrupts control flow the pc walks into nowhere, and
   the blocks entered just before that are what has to be inspected - so they
   are kept in a ring and dumped, once, with the guest code they were built
   from. Blocks reached by a direct jump from another block do not pass the
   dispatcher and are not in the ring. Set PSX_JIT_TRAP to 0 for the shipped
   firmware. */
#ifndef PSX_JIT_TRAP
#define PSX_JIT_TRAP 0
#endif

#if PSX_JIT_TRAP

#define JIT_TRACE_N 8

typedef struct
{
    uint32_t pc;
    uint32_t instr;
    uint32_t r31;
    uint32_t r29;
} jit_trace_t;

static jit_trace_t __attribute__((section(".bss.$SRAM_DTC"))) g_jit_trace[JIT_TRACE_N];
static uint32_t g_jit_trace_i;
static uint32_t g_jit_trapped;

static inline int jit_pc_sane(uint32_t pc)
{
    const uint32_t p = pc & 0x1fffffffu;

    if (p < 0x00200000u)
        return 1; /* RAM, all mirrors */

    if ((p >= 0x1fc00000u) && (p < 0x1fc80000u))
        return 1; /* BIOS */

    if ((p >= 0x1f800000u) && (p < 0x1f800400u))
        return 1; /* scratchpad */

    return 0;
}

static void jit_trap_dump(psx_cpu_t *cpu)
{
    g_jit_trapped = 1;

    PRINTF("\r\nJITTRAP pc=%08x next=%08x saved=%08x opcode=%08x branch=%u delay=%u load_d=%u load_v=%08x\r\n",
           (unsigned)cpu->pc, (unsigned)cpu->next_pc, (unsigned)cpu->saved_pc, (unsigned)cpu->opcode,
           (unsigned)cpu->branch, (unsigned)cpu->delay_slot, (unsigned)cpu->load_d, (unsigned)cpu->load_v);

    for (uint32_t i = 0; i < 32u; i += 4u)
    {
        PRINTF("  r%u %08x  r%u %08x  r%u %08x  r%u %08x\r\n",
               (unsigned)i, (unsigned)cpu->r[i], (unsigned)(i + 1u), (unsigned)cpu->r[i + 1u],
               (unsigned)(i + 2u), (unsigned)cpu->r[i + 2u], (unsigned)(i + 3u), (unsigned)cpu->r[i + 3u]);
    }

    /* oldest first */
    for (uint32_t k = 0; k < JIT_TRACE_N; k++)
    {
        const jit_trace_t *t = &g_jit_trace[(g_jit_trace_i + k) & (JIT_TRACE_N - 1u)];

        if (!t->pc)
            continue;

        PRINTF("BLK %08x n=%u ra=%08x sp=%08x\r\n",
               (unsigned)t->pc, (unsigned)t->instr, (unsigned)t->r31, (unsigned)t->r29);

        if (!jit_pc_sane(t->pc))
            continue;

        for (uint32_t i = 0; i < t->instr; i++)
        {
            PRINTF("   %08x: %08x\r\n",
                   (unsigned)(t->pc + i * 4u),
                   (unsigned)psx_bus_fast_read32(cpu->bus, t->pc + i * 4u));
        }
    }

    PRINTF("JITTRAP-END\r\n");
}

#endif

/* ------------------------------------------------------------------ dispatch */

uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_step(psx_cpu_t *cpu, uint32_t budget)
{
    const uint32_t sr = cpu->cop0_r[COP0_SR];

    /* Four pieces of state only the interpreter models, so while any of them is
       active the whole step runs interpreted - one instruction, then back here:

       - a pending interrupt: a block of natively translated instructions never
         looks at the interrupt state, so the exception is taken here;
       - an isolated cache (SR bit 16): stores must not reach RAM at all, and a
         natively translated store would write it. The BIOS runs its cache init
         loops this way, and letting those writes through corrupts the kernel
         data area;
       - a pending load: the value has to stay invisible for exactly one more
         instruction, and only the interpreter tracks that;
       - a pending branch: the next instruction is a delay slot, so what follows
         it is the branch target rather than the next address.

       Keeping these here rather than in every block is what lets the first
       instruction of a block be translated natively: the block can assume the
       plain, straight line case. Blocks that run one another directly keep to
       the same rule: the first two can only come about through an interpreted
       instruction or a device update, and the helpers make a block return here
       after the former; the last two are never left behind by an exit that
       hands over. */
    if ((sr & SR_ISC) || cpu->load_d || cpu->branch ||
        ((sr & SR_IEC) && (sr & cpu->cop0_r[COP0_CAUSE] & 0x00000700u)))
    {
        psx_cpu_cycle(cpu);

        g_jit_stats.dispatch_steps++;

        return cpu->last_cycles;
    }

    const uint32_t pc = cpu->pc;

#if PSX_JIT_TRAP
    if (!g_jit_trapped && !jit_pc_sane(pc))
        jit_trap_dump(cpu);
#endif

    uint32_t index = jit_lookup(pc);

    if (!index || !g_jit_meta[index - 1u].instr)
    {
        index = jit_compile(cpu, pc);

        if (!index)
        {
            /* could not translate: run a single instruction and try again */
            psx_cpu_cycle(cpu);

            return cpu->last_cycles;
        }
    }

    psx_jit_meta_t *const m = &g_jit_meta[index - 1u];

    if (m->heat != 0xffffu)
        m->heat++;

    g_jit_stats.dispatches++;

    if (!m->hot)
    {
        g_jit_stats.cold_dispatches++;

        if (++g_jit_cold_runs >= g_jit_retier_at)
            jit_retier();
    }

#if PSX_JIT_TRAP
    if (!g_jit_trapped)
    {
        jit_trace_t *t = &g_jit_trace[g_jit_trace_i];

        t->pc = pc;
        t->instr = m->instr;
        t->r31 = cpu->r[31];
        t->r29 = cpu->r[29];

        g_jit_trace_i = (g_jit_trace_i + 1u) & (JIT_TRACE_N - 1u);
    }
#endif

    g_jit_cycles = 0;

    const uint32_t native_cycles = psx_jit_enter(cpu, g_jit_link[index - 1u].code, budget, &g_jit_regs);

    /* Natively translated instructions never touch total_cycles, so the speed
       counters would only ever see the interpreted part of the work. */
    cpu->total_cycles += native_cycles;

    return g_jit_cycles + native_cycles;
}

const psx_jit_stats_t *psx_jit_get_stats(void)
{
    return &g_jit_stats;
}

void psx_jit_code_ranges(uint32_t out[4])
{
    out[0] = (uint32_t)(uintptr_t)&g_jit_code[0];
    out[1] = out[0] + PSX_JIT_CODE_SIZE;
    out[2] = (uint32_t)(uintptr_t)&g_jit_master[0];
    out[3] = out[2] + PSX_JIT_MASTER_SIZE;
}

#else /* !PSX_JIT_ENABLE */

uint8_t g_psx_jit_code_pages[1];

void psx_jit_init(void) {}
void psx_jit_reset(void) {}
void psx_jit_invalidate(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
void psx_jit_code_ranges(uint32_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }

uint32_t psx_jit_step(psx_cpu_t *cpu, uint32_t budget)
{
    (void)budget;

    psx_cpu_cycle(cpu);

    return cpu->last_cycles;
}

static psx_jit_stats_t g_jit_stats_off;

const psx_jit_stats_t *psx_jit_get_stats(void)
{
    return &g_jit_stats_off;
}

#endif
