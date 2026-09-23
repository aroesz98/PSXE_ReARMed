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
    uint32_t links; /* r8  */
    uint32_t exit;  /* r10 */
    uint32_t helpers[PSX_JIT_H_COUNT];
} psx_jit_regs_t;

/*
    Register jumps (JR / JALR, mostly function returns) find their block here
    without going back to the dispatcher: guest pc -> link offset, direct
    mapped. An entry stays right for as long as the link exists, which is until
    the whole code cache is emptied - a block that was invalidated keeps its
    link, which then leads out - so only psx_jit_reset() clears it.
*/
#define PSX_JIT_JRC_BITS 7u
#define PSX_JIT_JRC_SIZE (1u << PSX_JIT_JRC_BITS)

typedef struct
{
    uint32_t pc;     /* 1: empty (no guest pc is odd) */
    uint32_t offset; /* of the link */
} psx_jit_jrc_t;

static psx_jit_jrc_t __attribute__((section(".bss.$SRAM_DTC"), aligned(8))) g_jit_jrc[PSX_JIT_JRC_SIZE];

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
    to `code` (behind the budget test the block starts with). Blocks hand over
    to one another without coming back; whichever block stops - out of budget,
    a miss, a helper that leaves - returns to psx_jit_block_exit through r10.
    r5 counts the slice down, so what the blocks used is the budget (kept on the
    stack) minus r5.

    psx_jit_exit_link is the way out for a block that would have jumped into
    another one but may not (the budget is used up: the budget test of that
    block sends it here) or cannot (the link has no code): r3 is the offset of
    that link, and since a jump from block to block publishes nothing, the guest
    pc the link stands for is stored here, on the way out.
*/
uint32_t psx_jit_enter(psx_cpu_t *cpu, uint32_t code, uint32_t budget, const psx_jit_regs_t *regs);
void psx_jit_block_exit(void);
void psx_jit_exit_link(void);

_Static_assert((offsetof(psx_cpu_t, pc) == 132u) && (offsetof(psx_cpu_t, next_pc) == 136u),
               "the stubs store pc / next_pc at fixed offsets");
_Static_assert(PSX_JIT_H_PC_BASE == 44u, "psx_jit_exit_link reads the pc table from [r9, #44]");
_Static_assert(offsetof(psx_jit_regs_t, helpers) == 16u, "the entry stub points r9 at regs + 16");

__attribute__((naked, noinline, used, section(".ramfunc.$SRAM_ITC")))
uint32_t psx_jit_enter(psx_cpu_t *cpu, uint32_t code, uint32_t budget, const psx_jit_regs_t *regs)
{
    __asm__(
        "push {r2, r4-r11, lr}\n"
        "mov r4, r0\n"
        "mov r5, r2\n"
        "ldr r6, [r3, #0]\n"
        "ldr r7, [r3, #4]\n"
        "ldr r8, [r3, #8]\n"
        "ldr r10, [r3, #12]\n"
        "add r9, r3, #16\n"
        "bx r1\n"
        ".balign 4\n"
        ".global psx_jit_exit_link\n"
        ".thumb_func\n"
        ".type psx_jit_exit_link, %function\n"
        "psx_jit_exit_link:\n"
        "ldr r0, [r9, #44]\n"
        "ldr r0, [r0, r3]\n"
        "adds r1, r0, #4\n"
        "strd r0, r1, [r4, #132]\n"
        ".global psx_jit_block_exit\n"
        ".thumb_func\n"
        ".type psx_jit_block_exit, %function\n"
        "psx_jit_block_exit:\n"
        "ldr r0, [sp]\n"
        "subs r0, r0, r5\n"
        "pop {r2, r4-r11, pc}\n"
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

/* Where the interpreted instructions are: a small table that keeps the guest
   pcs seen most often (a slot is taken over once its count has decayed), for a
   look through the probe. Diagnostic, like the histogram. */
#define PSX_JIT_HIST_PCS 128u

typedef struct
{
    uint32_t pc;
    uint32_t count;
} psx_jit_hist_pc_t;

psx_jit_hist_pc_t __attribute__((section(".bss.$SRAM_DTC"), used)) g_jit_hist_pc[PSX_JIT_HIST_PCS];

static inline void jit_hist_note_pc(uint32_t pc)
{
    psx_jit_hist_pc_t *const t = &g_jit_hist_pc[(pc >> 2) & (PSX_JIT_HIST_PCS - 1u)];

    if (t->pc == pc)
        t->count++;
    else if (t->count)
        t->count--;
    else
    {
        t->pc = pc;
        t->count = 1;
    }
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
    jit_hist_note_pc(cpu->saved_pc);
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

/* where the branch a delay slot belongs to goes: r3 of the block, a link offset
   or a guest pc */
static inline uint32_t jit_branch_target(uint32_t r3, uint32_t is_link)
{
    return is_link ? g_jit_pc[r3 >> 2] : r3;
}

/*
    The slow path of a natively translated load or store (PSX_JIT_H_MEM): what the
    stub below does not serve itself - the scratchpad - comes here, with the
    address, the stub's data (the PSX_JIT_MD_* description) and the block's r3.

    Everything goes through the same bus functions the interpreter uses, so a
    device sees exactly the same accesses. A misaligned address is run by the
    interpreter (as a delay slot, with the branch pending, when it is one),
    which raises the address error; the block leaves after that.

    Returns the loaded value in the low word; the high word is 0 to go on in the
    block, or 1 + the cycles to take off r5 on the way out.
*/
static inline uint64_t jit_slow_leave(uint32_t cycles)
{
    return (uint64_t)(cycles + 1u) << 32;
}

/* The guest pc of a PSX_JIT_H_MEM stub: the stub's BL leads to the block's
   trampoline, which keeps the guest pc of the block's first instruction. */
static inline uint32_t jit_stub_pc(const uint32_t *data, uint32_t desc)
{
    const uint16_t *const bl = (const uint16_t *)(const void *)data - 2;

    const uint32_t h1 = bl[0];
    const uint32_t h2 = bl[1];
    const uint32_t s = (h1 >> 10) & 1u;
    const uint32_t i1 = ~(((h2 >> 13) & 1u) ^ s) & 1u;
    const uint32_t i2 = ~(((h2 >> 11) & 1u) ^ s) & 1u;

    const uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3ffu) << 12) | ((h2 & 0x7ffu) << 1);
    const int32_t off = ((int32_t)(imm << 7)) >> 7;

    /* the BL's pc is the address of the data word */
    const uint32_t *const tramp = (const uint32_t *)(uintptr_t)((uint32_t)(uintptr_t)data + (uint32_t)off);

    return tramp[1] + PSX_JIT_MD_INDEX(desc) * 4u;
}

uint64_t __attribute__((used, section(".ramfunc.$SRAM_ITC"))) psx_jit_mem_slow(psx_cpu_t *cpu, uint32_t addr,
                                                                              const uint32_t *data, uint32_t r3)
{
    const uint32_t desc = data[0];
    const uint32_t pc = jit_stub_pc(data, desc);
    const uint32_t size = PSX_JIT_MD_SIZE(desc);
    const uint32_t cycles = PSX_JIT_MD_CYCLES(desc);

    psx_bus_t *const bus = cpu->bus;

    g_jit_stats.slow_mem++;

    if (addr & (size - 1u))
    {
        /* An address error: the interpreter runs the instruction, and raises
           it - as a delay slot, with the branch pending, when it is one. The
           dispatcher could not be left to do it: it would run the same block
           again. The instruction's cycles are the interpreter's. */
        cpu->pc = pc;

        if (desc & PSX_JIT_MD_DELAY)
        {
            cpu->next_pc = jit_branch_target(r3, desc & PSX_JIT_MD_LINK);
            cpu->branch = 1;
        }
        else
        {
            cpu->next_pc = pc + 4u;
        }

        psx_cpu_cycle(cpu);

        g_jit_cycles += cpu->last_cycles;
        g_jit_stats.interp_steps++;

        return jit_slow_leave(cycles);
    }

    if (desc & PSX_JIT_MD_STORE)
    {
        const uint32_t v = cpu->r[PSX_JIT_MD_RT(desc)];

        /* RAM gets here only because code was translated from where it goes:
           the fast write invalidates it */
        if (size == 4u)
            psx_bus_fast_write32(bus, addr, v);
        else if (size == 2u)
            psx_bus_fast_write16(bus, addr, v);
        else
            psx_bus_fast_write8(bus, addr, v);

        g_jit_cycles += psx_bus_fast_take_cycles(bus);

        /* A device register was written: the slice this block runs in was
           sized from the devices' deadlines, and one of them may just have
           moved closer. psx_update looks at it again before anything else
           runs - the block leaves behind this instruction. */
        if (g_psx_bus_io_written)
        {
            if (desc & PSX_JIT_MD_DELAY)
            {
                const uint32_t t = jit_branch_target(r3, desc & PSX_JIT_MD_LINK);

                cpu->pc = t;
                cpu->next_pc = t + 4u;
            }
            else
            {
                cpu->pc = pc + 4u;
                cpu->next_pc = pc + 8u;
            }

            return jit_slow_leave(cycles + 2u);
        }

        return 0u;
    }

    uint32_t v;

    if (size == 4u)
        v = psx_bus_fast_read32(bus, addr);
    else if (size == 2u)
        v = (desc & PSX_JIT_MD_SIGNED) ? (uint32_t)(int32_t)(int16_t)psx_bus_fast_read16(bus, addr)
                                       : (uint32_t)psx_bus_fast_read16(bus, addr);
    else
        v = (desc & PSX_JIT_MD_SIGNED) ? (uint32_t)(int32_t)(int8_t)psx_bus_fast_read8(bus, addr)
                                       : (uint32_t)psx_bus_fast_read8(bus, addr);

    g_jit_cycles += psx_bus_fast_take_cycles(bus);

    return v;
}

/* LWC2 / SWC2 */
uint32_t __attribute__((used, section(".ramfunc.$SRAM_ITC"))) psx_jit_gte_transfer(psx_cpu_t *cpu, uint32_t pc,
                                                                                  uint32_t opcode)
{
    const uint32_t diverged = psx_cpu_gte_transfer(cpu, pc, opcode);

    g_jit_cycles += cpu->last_cycles;

    if (diverged)
        return 1u;

    /* the store may have been to a device register */
    return g_psx_bus_io_written ? 1u : 0u;
}

void psx_jit_interp_delay(psx_cpu_t *cpu, uint32_t pc, uint32_t next_pc);

/*
    The slow path of a natively translated LWC2 / SWC2 (PSX_JIT_H_GTE_MEM): r0 of
    the block is the address, the data the pc (with PSX_JIT_GF_* in the low
    bits) and the opcode. An aligned transfer of a plain register to or from
    the scratchpad is done right here and the block goes on; everything else is
    the interpreter's, and in a delay slot that leaves the block. Returns non zero
    to leave.
*/
uint32_t __attribute__((used, section(".ramfunc.$SRAM_ITC"))) psx_jit_gte_slow(psx_cpu_t *cpu, uint32_t addr,
                                                                              const uint32_t *data, uint32_t r3)
{
    const uint32_t pc = data[0] & ~3u;
    const uint32_t flags = data[0] & 3u;
    const uint32_t opcode = data[1];

    const uint32_t a = psx_bus_fast_mask(addr);

    if (!(addr & 3u) && ((a - PSX_SPAD_FAST_BASE) < PSX_SPAD_FAST_SIZE))
    {
        const psx_jit_gte_reg_t *const g = &g_psx_jit_gte_regs[PSX_RT(opcode)];
        uint8_t *const p = cpu->bus->scratchpad->buf + (a - PSX_SPAD_FAST_BASE);
        uint8_t *const f = (uint8_t *)cpu + g->off;

        if ((opcode >> 26) == 0x32u) /* LWC2 */
        {
            if (g->wr != PSX_GTE_WR_CALL)
            {
                const uint32_t v = *(const uint32_t *)p;

                if (g->wr == PSX_GTE_WR_32)
                    *(uint32_t *)f = v;
                else if (g->wr == PSX_GTE_WR_16)
                    *(uint16_t *)f = (uint16_t)v;

                return 0u;
            }
        }
        else if (g->rd != PSX_GTE_RD_CALL) /* SWC2 */
        {
            uint32_t v;

            if (g->rd == PSX_GTE_RD_32)
                v = *(const uint32_t *)f;
            else if (g->rd == PSX_GTE_RD_S16)
                v = (uint32_t)(int32_t) * (const int16_t *)f;
            else
                v = *(const uint16_t *)f;

            *(uint32_t *)p = v;

            return 0u;
        }
    }

    if (!(flags & PSX_JIT_GF_DELAY))
        return psx_jit_gte_transfer(cpu, pc, opcode);

    psx_jit_interp_delay(cpu, pc, jit_branch_target(r3, flags & PSX_JIT_GF_LINK));

    return 1u;
}

/* A delay slot the block could not run natively after all: the interpreter runs
   it with the branch pending, and leaves pc at the target (or at an exception
   vector). The block ends right after, so there is nothing to report. */
void __attribute__((used, section(".ramfunc.$SRAM_ITC"))) psx_jit_interp_delay(psx_cpu_t *cpu, uint32_t pc,
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
    jit_hist_note_pc(cpu->saved_pc);
#endif
}

/* PSX_JIT_H_BHOOK: what the interpreter does when it fetches from 0xb4 */
void __attribute__((used, section(".ramfunc.$SRAM_ITC"))) psx_jit_b_hook(psx_cpu_t *cpu)
{
    if (cpu->b_function_hook)
        cpu->b_function_hook(cpu);
}

uint32_t psx_jit_jr_miss(uint32_t pc);

/*
    The helper stubs. A block calls them through r9 with data words behind the
    call (lr points at them, plus the Thumb bit); they return behind the data,
    or - when the C function says so - leave the block through r10 themselves,
    which is why the block does not test anything after a call. r3 survives
    them: in a branch delay slot it says where the branch goes.
*/
_Static_assert((PSX_JIT_H_SPAD == 48u) && (PSX_JIT_H_JRC == 52u), "the stubs read [r9, #48] / [r9, #52]");
_Static_assert((PSX_JIT_MD_STORE == 0x100u) && (PSX_JIT_MD_SIGNED == 0x80u), "psx_jit_mem_stub tests these bits");
_Static_assert(PSX_JIT_JRC_BITS == 7u, "psx_jit_jr takes seven bits of the pc");
_Static_assert(sizeof(psx_jit_jrc_t) == 8u, "psx_jit_jr indexes the cache by eight");

__attribute__((naked, noinline, used, section(".ramfunc.$SRAM_ITC"))) void psx_jit_stubs(void)
{
    __asm__(
        /* H_INTERP: no data */
        ".global psx_jit_interp_stub\n"
        ".thumb_func\n"
        ".type psx_jit_interp_stub, %function\n"
        "psx_jit_interp_stub:\n"
        "push {r3, lr}\n"
        "mov r0, r4\n"
        "ldr r12, =psx_jit_interp_op\n"
        "blx r12\n"
        "pop {r3, lr}\n"
        "cbnz r0, 1f\n"
        "bx lr\n"
        "1:\n"
        "bx r10\n"

        /* H_INTERP_AT / H_LOAD_AT: .word pc */
        ".balign 4\n"
        ".global psx_jit_interp_at_stub\n"
        ".thumb_func\n"
        ".type psx_jit_interp_at_stub, %function\n"
        "psx_jit_interp_at_stub:\n"
        "ldr r12, =psx_jit_interp_op_at\n"
        "b 2f\n"
        ".global psx_jit_load_at_stub\n"
        ".thumb_func\n"
        ".type psx_jit_load_at_stub, %function\n"
        "psx_jit_load_at_stub:\n"
        "ldr r12, =psx_jit_interp_load_at\n"
        "2:\n"
        "push {r3, lr}\n"
        "ldr r1, [lr, #-1]\n"
        "mov r0, r4\n"
        "blx r12\n"
        "pop {r3, lr}\n"
        "cbnz r0, 1f\n"
        "add lr, lr, #4\n"
        "bx lr\n"
        "1:\n"
        "bx r10\n"

        /* H_DELAY: .word pc, .word flags; never comes back */
        ".balign 4\n"
        ".global psx_jit_delay_stub\n"
        ".thumb_func\n"
        ".type psx_jit_delay_stub, %function\n"
        "psx_jit_delay_stub:\n"
        "ldr r1, [lr, #-1]\n"
        "ldr r12, [lr, #3]\n"
        "mov r2, r3\n"
        "tst r12, #1\n"
        "beq 1f\n"
        "ldr r2, [r9, #44]\n"
        "ldr r2, [r2, r3]\n"
        "1:\n"
        "mov r0, r4\n"
        "ldr r12, =psx_jit_interp_delay\n"
        "blx r12\n"
        "bx r10\n"

        /* H_GTE_MEM: r0 guest address, .word pc | PSX_JIT_GF_*, .word opcode */
        ".balign 4\n"
        ".global psx_jit_gte_mem_stub\n"
        ".thumb_func\n"
        ".type psx_jit_gte_mem_stub, %function\n"
        "psx_jit_gte_mem_stub:\n"
        "push {r3, lr}\n"
        "mov r1, r0\n"
        "sub r2, lr, #1\n"
        "mov r0, r4\n"
        "ldr r12, =psx_jit_gte_slow\n"
        "blx r12\n"
        "pop {r3, lr}\n"
        "cbnz r0, 1f\n"
        "add lr, lr, #8\n"
        "bx lr\n"
        "1:\n"
        "bx r10\n"

        /*
            H_MEM: r0 guest address, .word desc. The scratchpad - FF7
            keeps its 3D working set there, some 165 000 accesses a second in a
            battle - is served right here in about twenty instructions and no
            stack; everything else goes to psx_jit_mem_slow. Back to the block
            (desc >> 22) halfwords before the end of the word, a loaded value
            in r2.
        */
        ".balign 4\n"
        ".global psx_jit_mem_stub\n"
        ".thumb_func\n"
        ".type psx_jit_mem_stub, %function\n"
        "psx_jit_mem_stub:\n"
        "ldr r12, [lr, #-1]\n"
        "bic r1, r0, #0xe0000000\n"
        "sub r1, r1, #0x1f800000\n"
        "cmp r1, #0x400\n"
        "bhs 9f\n"
        "tst r12, #0x40\n"
        "it ne\n"
        "tstne r0, #3\n"
        "bne 9f\n"
        "tst r12, #0x20\n"
        "it ne\n"
        "tstne r0, #1\n"
        "bne 9f\n"
        "ldr r2, [r9, #48]\n"
        "add r1, r1, r2\n"
        "tst r12, #0x100\n"
        "bne 5f\n"
        "tst r12, #0x40\n"
        "bne 3f\n"
        "tst r12, #0x20\n"
        "bne 4f\n"
        "tst r12, #0x80\n"
        "ite ne\n"
        "ldrsbne r2, [r1]\n"
        "ldrbeq r2, [r1]\n"
        "b 8f\n"
        "3:\n"
        "ldr r2, [r1]\n"
        "b 8f\n"
        "4:\n"
        "tst r12, #0x80\n"
        "ite ne\n"
        "ldrshne r2, [r1]\n"
        "ldrheq r2, [r1]\n"
        "b 8f\n"
        "5:\n"
        "and r2, r12, #0x1f\n"
        "ldr r2, [r4, r2, lsl #2]\n"
        "tst r12, #0x40\n"
        "bne 6f\n"
        "tst r12, #0x20\n"
        "ite ne\n"
        "strhne r2, [r1]\n"
        "strbeq r2, [r1]\n"
        "b 8f\n"
        "6:\n"
        "str r2, [r1]\n"
        "8:\n"
        "lsr r12, r12, #22\n"
        "add lr, lr, #4\n"
        "sub lr, lr, r12, lsl #1\n"
        "bx lr\n"
        "9:\n"
        "push {r3, lr}\n"
        "sub r2, lr, #1\n"
        "mov r1, r0\n"
        "mov r0, r4\n"
        "ldr r12, =psx_jit_mem_slow\n"
        "blx r12\n"
        "pop {r3, lr}\n"
        "cbnz r1, 7f\n"
        "mov r2, r0\n"
        "ldr r12, [lr, #-1]\n"
        "b 8b\n"
        "7:\n"
        "subs r1, r1, #1\n"
        "subs r5, r5, r1\n"
        "bx r10\n"

        /*
            H_JR, jumped to with a guest pc in r3 and the block's cycles already
            taken off r5: the block there, through the cache above, without the
            dispatcher. Its budget test gets the flags of CMP r5, #0 and r3 = its
            link offset, like after any other jump.
        */
        ".balign 4\n"
        ".global psx_jit_jr\n"
        ".thumb_func\n"
        ".type psx_jit_jr, %function\n"
        "psx_jit_jr:\n"
        "ldr r0, [r9, #52]\n"
        "ubfx r1, r3, #2, #7\n"
        "add r0, r0, r1, lsl #3\n"
        "ldrd r1, r2, [r0]\n"
        "cmp r1, r3\n"
        "bne 1f\n"
        "mov r3, r2\n"
        "cmp r5, #0\n"
        "ldr pc, [r8, r3]\n"
        "1:\n"
        "push {r0, r3}\n"
        "mov r0, r3\n"
        "ldr r12, =psx_jit_jr_miss\n"
        "blx r12\n"
        "pop {r1, r3}\n"
        "cbz r0, 2f\n"
        "subs r0, r0, #1\n"
        "strd r3, r0, [r1]\n"
        "mov r3, r0\n"
        "cmp r5, #0\n"
        "ldr pc, [r8, r3]\n"
        "2:\n"
        "adds r0, r3, #4\n"
        "strd r3, r0, [r4, #132]\n"
        "bx r10\n"
        ".ltorg\n"
    );
}

void psx_jit_interp_stub(void);
void psx_jit_interp_at_stub(void);
void psx_jit_load_at_stub(void);
void psx_jit_delay_stub(void);
void psx_jit_gte_mem_stub(void);
void psx_jit_mem_stub(void);
void psx_jit_jr(void);


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

/*
    A register jump whose target is not in the cache of psx_jit_jr: the link
    offset + 1 of the block there, made a link if there was none yet, or 0 when
    the dispatcher has to see it (a pc code is never taken from, a misaligned
    one, no room for another link).
*/
uint32_t __attribute__((used, section(".ramfunc.$SRAM_ITC"))) psx_jit_jr_miss(uint32_t pc)
{
    const uint32_t phys = pc & 0x1fffffffu;

    if ((pc & 3u) || ((phys >= 0x00200000u) && ((phys < 0x1fc00000u) || (phys >= 0x1fc80000u))))
        return 0;

    /* the dispatcher's compile needs three links of room: leave them to it */
    uint32_t i = jit_lookup(pc);

    if (!i && ((g_jit_block_count + 4u) <= PSX_JIT_MAX_BLOCKS))
        i = jit_get_block(pc);

    return i ? ((i - 1u) * 4u + 1u) : 0u;
}

static inline void jit_data_words_forget(void);

void psx_jit_reset(void)
{
    memset(g_jit_hash, 0, sizeof(g_jit_hash));
    memset(g_jit_page_head, 0, sizeof(g_jit_page_head));
    memset(g_jit_page_cells, 0, sizeof(g_jit_page_cells));
    memset(g_psx_jit_code_pages, 0, sizeof(g_psx_jit_code_pages));

    /* the links are numbered afresh */
    for (uint32_t i = 0; i < PSX_JIT_JRC_SIZE; i++)
    {
        g_jit_jrc[i].pc = 1u;
        g_jit_jrc[i].offset = 0u;
    }

    jit_data_words_forget();

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
    g_jit_regs.links = (uint32_t)(uintptr_t)g_jit_link;
    g_jit_regs.exit = (uint32_t)(uintptr_t)&psx_jit_block_exit | 1u;

    g_jit_regs.helpers[PSX_JIT_H_INTERP / 4u] = (uint32_t)(uintptr_t)&psx_jit_interp_stub | 1u;
    g_jit_regs.helpers[PSX_JIT_H_INTERP_AT / 4u] = (uint32_t)(uintptr_t)&psx_jit_interp_at_stub | 1u;
    g_jit_regs.helpers[PSX_JIT_H_LOAD_AT / 4u] = (uint32_t)(uintptr_t)&psx_jit_load_at_stub | 1u;
    g_jit_regs.helpers[PSX_JIT_H_DELAY / 4u] = (uint32_t)(uintptr_t)&psx_jit_delay_stub | 1u;
    g_jit_regs.helpers[PSX_JIT_H_GTE / 4u] = (uint32_t)(uintptr_t)&psx_cpu_gte_command;
    g_jit_regs.helpers[PSX_JIT_H_GTE_READ / 4u] = (uint32_t)(uintptr_t)&psx_cpu_gte_read;
    g_jit_regs.helpers[PSX_JIT_H_GTE_WRITE / 4u] = (uint32_t)(uintptr_t)&psx_cpu_gte_write;
    g_jit_regs.helpers[PSX_JIT_H_GTE_MEM / 4u] = (uint32_t)(uintptr_t)&psx_jit_gte_mem_stub | 1u;
    g_jit_regs.helpers[PSX_JIT_H_MEM / 4u] = (uint32_t)(uintptr_t)&psx_jit_mem_stub | 1u;
    g_jit_regs.helpers[PSX_JIT_H_EXIT_LINK / 4u] = (uint32_t)(uintptr_t)&psx_jit_exit_link | 1u;
    g_jit_regs.helpers[PSX_JIT_H_JR / 4u] = (uint32_t)(uintptr_t)&psx_jit_jr | 1u;
    g_jit_regs.helpers[PSX_JIT_H_PC_BASE / 4u] = (uint32_t)(uintptr_t)g_jit_pc;
    g_jit_regs.helpers[PSX_JIT_H_JRC / 4u] = (uint32_t)(uintptr_t)g_jit_jrc;
    g_jit_regs.helpers[PSX_JIT_H_BHOOK / 4u] = (uint32_t)(uintptr_t)&psx_jit_b_hook;

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

/*
    Words that are known to hold no code although their 32 byte cell does.

    The cell map cannot tell a variable from the function it sits right behind,
    and for a store into such a cell every block of the page has to be looked at
    - some forty of them, each with its record in SDRAM. In an FF7 battle that
    was 1.4 million blocks looked at in thirty seconds, to find the 42 that had
    really been overwritten: one per cent of the machine. So a store that turned
    out to touch no block leaves its word here, and the next store to it is
    waved through. Whatever is translated next may well be at one of these
    words, so translating anything forgets them all.
*/
#define PSX_JIT_DATA_WORDS 64u

static uint32_t __attribute__((section(".bss.$SRAM_DTC"))) g_jit_data_word[PSX_JIT_DATA_WORDS]; /* address | 1 */

static inline void jit_data_words_forget(void)
{
    memset(g_jit_data_word, 0, sizeof(g_jit_data_word));
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_invalidate(uint32_t addr, uint32_t size)
{
    if ((addr & 0x1fffffffu) >= 0x200000u)
        return; /* not guest RAM: nothing can have been compiled from it */

    const uint32_t lo = addr & 0x1fffffu;
    const uint32_t hi = lo + (size ? size : 1u); /* exclusive */

    /* a store that stays inside one word: the only kind worth remembering */
    const uint32_t word = lo & ~3u;
    const uint32_t slot = (word >> 2) & (PSX_JIT_DATA_WORDS - 1u);
    const int small = (hi <= (word + 4u));

    if (small && (g_jit_data_word[slot] == (word | 1u)))
        return;

    int word_clear = 1;
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
                word_clear = 0;
            }
            else
            {
                if ((b_lo < (word + 4u)) && ((b_lo + b_len) > word))
                    word_clear = 0; /* code in the rest of the word */

                cells |= jit_cell_mask(b_lo, b_len);
                link_to_me = &m->page_next;
            }

            i = next;
        }

        if (killed)
            g_jit_stats.invalidations++;

        /* what is left of the page */
        jit_page_set_cells(p, cells);

        if (small && word_clear)
            g_jit_data_word[slot] = word | 1u;
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

    /* the offset of the link in the table r8 points at, + 1 */
    return i ? ((i - 1u) * 4u + 1u) : 0u;
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
    g_jit_regs.helpers[PSX_JIT_H_SPAD / 4u] = (uint32_t)(uintptr_t)cpu->bus->scratchpad->buf;

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

        jit_data_words_forget(); /* this block may cover one of them */
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

    /* in behind the budget test the block starts with: this block runs at
       least once, whatever the budget */
    const uint32_t native_cycles =
        psx_jit_enter(cpu, g_jit_link[index - 1u].code + PSX_JIT_ENTRY_TEST_BYTES, budget, &g_jit_regs);

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
