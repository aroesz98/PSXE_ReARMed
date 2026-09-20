/*
    Recompiler core: code cache, block lookup, translation and dispatch.

    Phase 1 translates every guest instruction into a call to the interpreter
    step. That is deliberately slower than the plain interpreter, but it puts
    the whole machinery (cache, block management, invalidation, exit protocol)
    under test with behaviour that is identical to the interpreter by
    construction. Native instruction translation is added on top of this
    structure, one opcode class at a time, without touching the protocol.
*/

#include <stdint.h>
#include <string.h>

#include "jit.h"
#include "jit_emit.h"
#include "jit_translate.h"

#include "../bus.h"
#include "../bus_init.h"
#include "../bus_fast.h"
#include "fsl_debug_console.h"

#if PSX_JIT_ENABLE

/* ------------------------------------------------------------------ memory */

/* Generated code runs from ITCM: no instruction cache maintenance is needed,
   a data barrier is enough. */
static uint8_t __attribute__((section(".bss.$SRAM_ITC"), aligned(8))) g_jit_code[PSX_JIT_CODE_SIZE];

/* Block bookkeeping is touched once per block execution, so OCRAM (cached) is
   the right place for it - DTCM is reserved for what the inner loops touch. */
static psx_jit_block_t __attribute__((section(".bss.$SRAM_OC"))) g_jit_blocks[PSX_JIT_MAX_BLOCKS];
static psx_jit_block_t *__attribute__((section(".bss.$SRAM_OC"))) g_jit_hash[PSX_JIT_HASH_SIZE];

/* 1 KB page granularity over the 2 MB of guest RAM. Coarser pages caused code
   and data that merely live close to each other to invalidate one another. */
#define PSX_JIT_PAGE_SHIFT 10
#define PSX_JIT_PAGE_COUNT (0x200000u >> PSX_JIT_PAGE_SHIFT)

static psx_jit_block_t *__attribute__((section(".bss.$SRAM_OC"))) g_jit_page_head[PSX_JIT_PAGE_COUNT];

/* Shared with the memory write fast path (see bus_fast.h): one byte per page,
   checked on every guest store, so this one stays in DTCM. */
uint8_t __attribute__((section(".bss.$SRAM_DTC"))) g_psx_jit_code_pages[PSX_JIT_PAGE_COUNT];

static uint32_t g_jit_block_count;
static uint32_t g_jit_code_used;
static psx_jit_stats_t g_jit_stats;

/* Emulated cycles accumulated by the fallback inside the running block */
static uint32_t g_jit_cycles;

typedef uint32_t (*psx_jit_fn)(psx_cpu_t *cpu);

/* ------------------------------------------------------------------ helpers */

/* Runs exactly one guest instruction through the interpreter and reports
   whether control flow left the straight line the block was compiled for.
   Re-fetching the opcode here is what makes self modifying code safe. */
/* Slow path helpers for guest memory accesses that are not plain RAM. They are
   plain C calls, so the generated code only needs to pass the address. */
uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_read32_slow(psx_cpu_t *cpu, uint32_t addr)
{
    const uint32_t value = psx_bus_fast_read32(cpu->bus, addr);

    g_jit_cycles += psx_bus_fast_take_cycles(cpu->bus);

    return value;
}

uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_read16_slow(psx_cpu_t *cpu, uint32_t addr)
{
    const uint32_t value = psx_bus_fast_read16(cpu->bus, addr);

    g_jit_cycles += psx_bus_fast_take_cycles(cpu->bus);

    return value;
}

uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_read8_slow(psx_cpu_t *cpu, uint32_t addr)
{
    const uint32_t value = psx_bus_fast_read8(cpu->bus, addr);

    g_jit_cycles += psx_bus_fast_take_cycles(cpu->bus);

    return value;
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_write32_slow(psx_cpu_t *cpu, uint32_t addr, uint32_t value)
{
    psx_bus_fast_write32(cpu->bus, addr, value);

    g_jit_cycles += psx_bus_fast_take_cycles(cpu->bus);
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_write16_slow(psx_cpu_t *cpu, uint32_t addr, uint32_t value)
{
    psx_bus_fast_write16(cpu->bus, addr, value);

    g_jit_cycles += psx_bus_fast_take_cycles(cpu->bus);
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_write8_slow(psx_cpu_t *cpu, uint32_t addr, uint32_t value)
{
    psx_bus_fast_write8(cpu->bus, addr, value);

    g_jit_cycles += psx_bus_fast_take_cycles(cpu->bus);
}

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

/* Runs one instruction in the interpreter and reports whether control flow left
   the straight line: cpu->saved_pc is the address the interpreter fetched from,
   so the block does not have to pass the expected pc in. */
uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_interp_op(psx_cpu_t *cpu)
{
    psx_cpu_cycle(cpu);

    g_jit_cycles += cpu->last_cycles;
    g_jit_stats.interp_steps++;

#if PSX_JIT_HIST
    jit_hist_note(cpu->opcode);
#endif

    return (cpu->pc != (cpu->saved_pc + 4u)) ? 1u : 0u;
}

/* Escape path of a natively translated load: the address turned out not to be
   plain RAM, so the interpreter runs the load - which leaves the value in the
   load delay slot. The instruction that follows is native code and would never
   apply it, so it is applied here. That is equivalent: the translator only
   takes the native path when the following instruction neither reads nor writes
   the loaded register, so nobody can observe the earlier write back. */
uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_interp_load(psx_cpu_t *cpu)
{
    psx_cpu_cycle(cpu);

    g_jit_cycles += cpu->last_cycles;
    g_jit_stats.interp_steps++;

    if (cpu->pc != (cpu->saved_pc + 4u))
        return 1u; /* exception or branch: leave the load pending, block exits */

    cpu->r[cpu->load_d] = cpu->load_v;
    cpu->r[0] = 0;
    cpu->load_v = 0xffffffffu;
    cpu->load_d = 0;

    return 0u;
}

/* ------------------------------------------------------------------ lookup */

static inline uint32_t jit_hash(uint32_t pc)
{
    return (pc >> 2) & (PSX_JIT_HASH_SIZE - 1u);
}

static inline psx_jit_block_t *jit_lookup(uint32_t pc)
{
    psx_jit_block_t *b = g_jit_hash[jit_hash(pc)];

    while (b)
    {
        if (b->pc == pc)
            return b;

        b = b->next;
    }

    return 0;
}

void psx_jit_reset(void)
{
    memset(g_jit_hash, 0, sizeof(g_jit_hash));
    memset(g_jit_page_head, 0, sizeof(g_jit_page_head));
    memset(g_psx_jit_code_pages, 0, sizeof(g_psx_jit_code_pages));

    g_jit_block_count = 0;
    g_jit_code_used = 0;

    g_jit_stats.blocks = 0;
    g_jit_stats.code_used = 0;
    g_jit_stats.flushes++;

    __asm volatile("dsb\n\tisb" ::: "memory");
}

void psx_jit_init(void)
{
    memset(&g_jit_stats, 0, sizeof(g_jit_stats));

    psx_jit_reset();

    g_jit_stats.flushes = 0;
}

/* Unlinks one block from the lookup structures. The code it occupies is not
   reclaimed - the cache is compacted by a full flush when it runs out. */
static void jit_kill_block(psx_jit_block_t *b)
{
    psx_jit_block_t **pp = &g_jit_hash[jit_hash(b->pc)];

    while (*pp)
    {
        if (*pp == b)
        {
            *pp = b->next;
            break;
        }

        pp = &(*pp)->next;
    }

    b->pc = 0xffffffffu;
    b->code = 0;

    g_jit_stats.killed++;
}

void __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_invalidate(uint32_t addr, uint32_t size)
{
    if ((addr & 0x1fffffffu) >= 0x200000u)
        return; /* not guest RAM: nothing can have been compiled from it */

    const uint32_t masked = addr & 0x1fffffu;
    const uint32_t first = masked >> PSX_JIT_PAGE_SHIFT;
    const uint32_t last = (masked + (size ? (size - 1u) : 0u)) >> PSX_JIT_PAGE_SHIFT;

    for (uint32_t p = first; (p <= last) && (p < PSX_JIT_PAGE_COUNT); p++)
    {
        if (!g_psx_jit_code_pages[p])
            continue;

        g_jit_stats.invalidations++;

        psx_jit_block_t *b = g_jit_page_head[p];

        while (b)
        {
            psx_jit_block_t *next = b->page_next;

            jit_kill_block(b);

            b = next;
        }

        g_jit_page_head[p] = 0;
        g_psx_jit_code_pages[p] = 0;
    }
}

/* ------------------------------------------------------------------ compile */

static psx_jit_block_t *jit_alloc_block(uint32_t pc)
{
    if (g_jit_block_count >= PSX_JIT_MAX_BLOCKS)
        return 0;

    psx_jit_block_t *b = &g_jit_blocks[g_jit_block_count++];

    b->pc = pc;
    b->instr = 0;
    b->page = 0;
    b->code = 0;
    b->page_next = 0;
    b->next = g_jit_hash[jit_hash(pc)];

    g_jit_hash[jit_hash(pc)] = b;
    g_jit_stats.blocks = g_jit_block_count;

    return b;
}

static psx_jit_block_t *jit_compile(psx_cpu_t *cpu, uint32_t pc)
{
    psx_emit_t e;
    uint16_t *exits[PSX_JIT_MAX_INSTR * 2 + 4];
    uint32_t exit_count = 0;

    if ((g_jit_block_count >= PSX_JIT_MAX_BLOCKS) ||
        ((PSX_JIT_CODE_SIZE - g_jit_code_used) < 2048u))
    {
        psx_jit_reset();
    }

    psx_jit_block_t *b = jit_alloc_block(pc);

    if (!b)
        return 0;

    psx_emit_init(&e, &g_jit_code[g_jit_code_used], PSX_JIT_CODE_SIZE - g_jit_code_used);

    /* prologue */
    psx_emit_push(&e, (1u << PSX_R4) | (1u << PSX_R5) | (1u << PSX_R6) | (1u << PSX_R7) | (1u << PSX_LR));
    psx_emit_mov(&e, PSX_JIT_CPU, PSX_R0);
    psx_emit_mov_imm8(&e, PSX_JIT_CYC, 0);
    psx_emit_imm32(&e, PSX_JIT_RAM, (uint32_t)(uintptr_t)cpu->bus->ram->buf);
    psx_emit_imm32(&e, PSX_JIT_PAGES, (uint32_t)(uintptr_t)g_psx_jit_code_pages);

    psx_jit_ctx_t ctx;

    ctx.e = &e;
    ctx.helper_interp = (uint32_t)(uintptr_t)&psx_jit_interp_op;
    ctx.helper_mem = (uint32_t)(uintptr_t)&psx_jit_interp_load;
    ctx.exits = exits;
    ctx.exit_count = &exit_count;
    ctx.exit_max = (uint32_t)(sizeof(exits) / sizeof(exits[0]));
    ctx.force_next = 0;
    ctx.cycles_done = 0;
    ctx.helper_gte = (uint32_t)(uintptr_t)&psx_cpu_gte_command;
    ctx.bus = cpu->bus;

    uint32_t guest = pc;
    uint32_t count = 0;
    uint32_t native = 0;
    uint32_t pending_cycles = 0;

    /* The dispatcher guarantees there is no pending load and no pending branch
       when a block is entered, so the first instruction needs no special care. */
    int force_interp = 0;

    /* Natively translated instructions do not advance pc / next_pc */
    int state_stale = 0;

    const uint32_t page_end = (pc | ((1u << PSX_JIT_PAGE_SHIFT) - 1u)) + 1u;

    uint32_t op = psx_bus_fast_read32(cpu->bus, guest);

    while ((count < PSX_JIT_MAX_INSTR) && (guest < page_end) && !e.overflow)
    {
        const uint32_t next_op = ((guest + 4u) < page_end)
                                     ? psx_bus_fast_read32(cpu->bus, guest + 4u)
                                     : 0xffffffffu; /* unknown: treated as "uses everything" */

        ctx.guest = guest;
        ctx.next_op = next_op;

        /* The interpreter fires the BIOS TTY hook when it fetches from 0xb4, so
           that instruction - and the branch it may be the delay slot of - stays
           interpreted. Decided at compile time, so it costs nothing to run. */
        if ((((guest & 0x3fffffffu) == 0x000000b4u)) ||
            (((guest + 4u) & 0x3fffffffu) == 0x000000b4u))
            force_interp = 1;

        int emitted = 0;
        int ended = 0;

        if (!force_interp)
        {
            emitted = psx_jit_translate_alu(&e, op);

            if (!emitted)
            {
                /* Anything that can escape to the interpreter needs the cycle
                   counter flushed first: the interpreter adds its own. */
                if (pending_cycles)
                {
                    psx_emit_add_imm12(&e, PSX_JIT_CYC, PSX_JIT_CYC, pending_cycles);
                    pending_cycles = 0;
                }

                emitted = psx_jit_translate_mem(&ctx, op);

                if (!emitted)
                    emitted = psx_jit_translate_trap_alu(&ctx, op);

                if (!emitted)
                    emitted = psx_jit_translate_div(&ctx, op);

                if (!emitted)
                    emitted = psx_jit_translate_cop2(&ctx, op);
            }

            /* A branch is translated together with its delay slot and always
               ends the block, so both instructions have to fit in it. */
            if (!emitted && ((count + 2u) <= PSX_JIT_MAX_INSTR) && ((guest + 4u) < page_end))
            {
                emitted = psx_jit_translate_branch(&ctx, op, next_op);
                ended = emitted;
            }
        }

        if (ended)
        {
            /* branch plus delay slot: two instructions, and the pc the block
               leaves behind has already been stored */
            pending_cycles += 4u;
            native += 2u;
            guest += 8u;
            count += 2u;
            state_stale = 0;

            break;
        }

        if (emitted)
        {
            /* a GTE command adds its own, variable, cycle count */
            if (!ctx.cycles_done)
                pending_cycles += 2u;

            native++;
            force_interp = ctx.force_next;
            state_stale = 1;
            ctx.force_next = 0;
            ctx.cycles_done = 0;
        }
        else
        {
            if (pending_cycles)
            {
                psx_emit_add_imm12(&e, PSX_JIT_CYC, PSX_JIT_CYC, pending_cycles);
                pending_cycles = 0;
            }

            /* The interpreter fetches from cpu->pc, so publish it when the
               natively translated instructions left it behind. It must not be
               touched when the previous instruction was interpreted: it may
               have set up a branch (pc = delay slot, next_pc = target). */
            psx_jit_emit_interp_one(&ctx, state_stale);

            state_stale = 0;

            /* After a branch the next instruction is its delay slot and only
               the interpreter keeps pc / next_pc consistent through it; after a
               load the next instruction must observe the load delay slot. */
            force_interp = psx_jit_is_branch(op) || psx_jit_leaves_pending_load(op);
        }

        guest += 4u;
        count++;
        op = next_op;
    }

    if (pending_cycles)
        psx_emit_add_imm12(&e, PSX_JIT_CYC, PSX_JIT_CYC, pending_cycles);

    /* Straight line end: publish where the block stopped, unless the last
       instruction was interpreted (it already left pc / next_pc correct,
       including branch targets). */
    if (state_stale)
        psx_jit_publish_pc(&e, guest);

    /* exits from the middle land here: pc is already correct */
    const uint32_t epilogue = psx_emit_here(&e);

    for (uint32_t i = 0; i < exit_count; i++)
        psx_emit_patch_bcond(exits[i], PSX_CC_NE, epilogue);

    psx_emit_mov(&e, PSX_R0, PSX_JIT_CYC);
    psx_emit_pop(&e, (1u << PSX_R4) | (1u << PSX_R5) | (1u << PSX_R6) | (1u << PSX_R7) | (1u << PSX_PC));

    if (e.overflow || (count == 0))
    {
        psx_jit_reset();
        return 0;
    }

    b->instr = (uint16_t)count;
    b->code = (uint32_t)(uintptr_t)e.start | 1u; /* Thumb */

    g_jit_code_used += psx_emit_size(&e);
    g_jit_stats.code_used = g_jit_code_used;
    g_jit_stats.compiles++;
    g_jit_stats.native += native;

    /* Register the block with the page it was translated from, so a store into
       that page only drops the blocks actually built from it. A block never
       spans a page: translation stops at the page boundary. */
    const uint32_t phys = pc & 0x1fffffffu;

    if (phys < 0x200000u)
    {
        const uint32_t page = (phys & 0x1fffffu) >> PSX_JIT_PAGE_SHIFT;

        b->page = (uint16_t)page;
        b->page_next = g_jit_page_head[page];

        g_jit_page_head[page] = b;
        g_psx_jit_code_pages[page] = 1;
    }

    /* the code was written through the data path, make it visible to fetch */
    __asm volatile("dsb\n\tisb" ::: "memory");

    return b;
}

/* ------------------------------------------------------------------ trap */

/* Diagnostic: the guest pc must always point at RAM, BIOS or the scratchpad.
   When a translation bug corrupts control flow the pc walks into nowhere, and
   the blocks executed just before that are what has to be inspected - so they
   are kept in a ring and dumped, once, with the guest code they were built
   from. Set PSX_JIT_TRAP to 0 for the shipped firmware. */
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

uint32_t __attribute__((section(".ramfunc.$SRAM_ITC"))) psx_jit_step(psx_cpu_t *cpu)
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
       plain, straight line case. */
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

    psx_jit_block_t *b = jit_lookup(pc);

    if (!b)
    {
        b = jit_compile(cpu, pc);

        if (!b)
        {
            /* could not translate: run a single instruction and try again */
            psx_cpu_cycle(cpu);

            return cpu->last_cycles;
        }
    }

#if PSX_JIT_TRAP
    if (!g_jit_trapped)
    {
        jit_trace_t *t = &g_jit_trace[g_jit_trace_i];

        t->pc = pc;
        t->instr = b->instr;
        t->r31 = cpu->r[31];
        t->r29 = cpu->r[29];

        g_jit_trace_i = (g_jit_trace_i + 1u) & (JIT_TRACE_N - 1u);
    }
#endif

    g_jit_cycles = 0;

    const uint32_t native_cycles = ((psx_jit_fn)(uintptr_t)b->code)(cpu);

    /* Natively translated instructions never touch total_cycles, so the speed
       counters would only ever see the interpreted part of the work. */
    cpu->total_cycles += native_cycles;

    return g_jit_cycles + native_cycles;
}

const psx_jit_stats_t *psx_jit_get_stats(void)
{
    return &g_jit_stats;
}

#else /* !PSX_JIT_ENABLE */

uint8_t g_psx_jit_code_pages[1];

void psx_jit_init(void) {}
void psx_jit_reset(void) {}
void psx_jit_invalidate(uint32_t addr, uint32_t size) { (void)addr; (void)size; }

uint32_t psx_jit_step(psx_cpu_t *cpu)
{
    psx_cpu_cycle(cpu);

    return cpu->last_cycles;
}

static psx_jit_stats_t g_jit_stats_off;

const psx_jit_stats_t *psx_jit_get_stats(void)
{
    return &g_jit_stats_off;
}

#endif
