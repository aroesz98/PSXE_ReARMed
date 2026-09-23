#ifndef PSX_JIT_BLOCK_H
#define PSX_JIT_BLOCK_H

/*
    Builds one block: the translation of a straight run of guest instructions,
    up to and including the first branch and its delay slot.

    This is everything that decides what a block looks like, kept free of the
    code cache and of the hardware so that the same code can be built on a host
    and its output run under an ARM emulator against a reference interpreter.

    Layout of a block:

        IT LE ; LDR.W PC, [r9, #EXIT_LINK]   the budget test, see jit_translate.h
        ...                                  the instructions, straight through
        SUBS r5, #cycles ; LDR.W PC, [r8, r3] (or the register jump helper, or
                                             the pc and back to the dispatcher)
        stubs                                the slow paths, one per access or
                                             trapping instruction
        trampolines                          LDR.W PC, [r9, #slot], one per
                                             helper the block calls with a BL
*/

#include <stdint.h>

#include "jit_emit.h"
#include "jit_translate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Instructions per block, see PSX_JIT_MAX_INSTR in jit.h */
#define PSX_JIT_BLOCK_MAX_INSTR 30u /* the stub description has five bits for the index */

typedef struct
{
    uint32_t instr;  /* guest instructions the block covers */
    uint32_t native; /* how many of them were translated natively */
} psx_jit_block_info_t;

/* Lays out the stubs behind the block and points the branches at them */
static inline void psx_jit_emit_stubs(psx_jit_ctx_t *c)
{
    psx_emit_t *const e = c->e;

    for (uint32_t i = 0; (i < c->stub_count) && !e->overflow; i++)
    {
        const psx_jit_stub_t *const s = &c->stubs[i];

        if (!s->site[0] && !s->site[1] && !s->site[2])
            continue; /* nothing leads here */

        /* where the data lands decides the padding, so the start is known only
           once the rest of the stub is: count what comes before the call */
        uint32_t lead = 0;

        if (s->kind == PSX_JIT_STUB_LEAVE)
        {
            /* no call, no data: the cycles (the instruction's own too: it has
               run), the pc behind it, out */
            const uint32_t start = psx_emit_here(e);

            for (uint32_t k = 0; k < 2u; k++)
            {
                if (!s->site[k])
                    continue;

                const uint32_t dist = start - ((uint32_t)(uintptr_t)s->site[k] + 4u);

                if (!s->narrow)
                    psx_emit_patch_bcond(s->site[k], s->cond[k], start);
                else if (dist > 254u)
                {
                    c->range_fail = 1;
                    e->overflow = 1;
                    return;
                }
                else
                    psx_emit_patch_bcond_short(e, s->site[k], s->cond[k], start);
            }

            psx_emit_subs_imm(e, PSX_JIT_CYC, PSX_JIT_CYC, s->cycles + 2u);

            psx_jit_publish_pc(e, s->pc + 4u);
            psx_emit_bx(e, PSX_JIT_EXIT);
            continue;
        }

        if (s->set_addr)
        {
            psx_emit_t probe;
            uint16_t tmp[4];

            psx_emit_init(&probe, tmp, sizeof(tmp));
            psx_emit_const(&probe, PSX_R0, s->addr);
            lead += psx_emit_size(&probe);
        }

        const uint32_t sub_cycles = (s->kind == PSX_JIT_STUB_MEM) ? 0u : s->cycles;

        if (sub_cycles)
            lead += (sub_cycles <= 255u) ? 2u : 4u;

        /* the BL is four bytes: the data follows right behind it */
        if (((psx_emit_here(e) + lead) & 3u) != 0u)
            psx_emit16(e, 0xbf00u); /* NOP, so the data is word aligned */

        const uint32_t start = psx_emit_here(e);

        for (uint32_t k = 0; k < 3u; k++)
        {
            if (!s->site[k])
                continue;

            if (!s->narrow)
            {
                if (s->cond[k] == PSX_CC_AL)
                    psx_emit_patch_b(s->site[k], start);
                else
                    psx_emit_patch_bcond(s->site[k], s->cond[k], start);

                continue;
            }

            /* out of reach of the 16 bit branch: the block is built again */
            const uint32_t dist = start - ((uint32_t)(uintptr_t)s->site[k] + 4u);

            if (dist > ((s->cond[k] == PSX_CC_AL) ? 2046u : 254u))
            {
                c->range_fail = 1;
                e->overflow = 1;
                return;
            }

            if (s->cond[k] == PSX_CC_AL)
                psx_emit_patch_b_short(e, s->site[k], start);
            else
                psx_emit_patch_bcond_short(e, s->site[k], s->cond[k], start);
        }

        if (s->set_addr)
            psx_emit_const(e, PSX_R0, s->addr);

        if (sub_cycles)
            psx_emit_subs_imm(e, PSX_JIT_CYC, PSX_JIT_CYC, sub_cycles);

        switch (s->kind)
        {
        case PSX_JIT_STUB_MEM:
        {
            psx_jit_emit_call(c, PSX_JIT_H_MEM);

            const uint32_t data = psx_emit_here(e);

            /* back to "resume", counted from the end of the data */
            const uint32_t back = (data + 4u - (uint32_t)(uintptr_t)s->resume) >> 1;

            if ((back >= 1024u) || (s->cycles > 63u))
                e->overflow = 1;

            psx_emit_data32(e, s->data | (s->cycles << 16) | (back << 22));
            break;
        }

        case PSX_JIT_STUB_TRAP:
            /* the interpreter raises the exception and the helper leaves */
            psx_jit_emit_call(c, PSX_JIT_H_INTERP_AT);
            psx_emit_data32(e, s->pc);
            psx_emit_bx(e, PSX_JIT_EXIT); /* not reached */
            break;

        case PSX_JIT_STUB_DELAY:
            psx_jit_emit_call(c, PSX_JIT_H_DELAY);
            psx_emit_data32(e, s->pc);
            psx_emit_data32(e, s->data);
            break;

        case PSX_JIT_STUB_GTE_MEM:
        default:
        {
            psx_jit_emit_call(c, PSX_JIT_H_GTE_MEM);
            psx_emit_data32(e, s->pc);
            psx_emit_data32(e, s->data);

            /* the cycles go back on: the block takes them off itself */
            if (sub_cycles)
                psx_emit_add_imm12(e, PSX_JIT_CYC, PSX_JIT_CYC, sub_cycles);

            psx_emit_b(e, (uint32_t)(uintptr_t)s->resume);
            break;
        }
        }
    }
}

/* BL <target>, written over a placeholder */
static inline void psx_jit_patch_bl(uint16_t *slot, uint32_t target)
{
    const int32_t off = (int32_t)(target - ((uint32_t)(uintptr_t)slot + 4u));
    const uint32_t s = ((uint32_t)off >> 24) & 1u;
    const uint32_t i1 = ((uint32_t)off >> 23) & 1u;
    const uint32_t i2 = ((uint32_t)off >> 22) & 1u;
    const uint32_t j1 = (~i1 ^ s) & 1u;
    const uint32_t j2 = (~i2 ^ s) & 1u;

    slot[0] = (uint16_t)(0xf000u | (s << 10) | (((uint32_t)off >> 12) & 0x3ffu));
    slot[1] = (uint16_t)(0xd000u | (j1 << 13) | (j2 << 11) | (((uint32_t)off >> 1) & 0x7ffu));
}

/*
    The trampolines, the very last thing in the block: one LDR.W PC, [r9, #slot]
    for every helper the block calls, and the BLs pointed at them. The one of
    PSX_JIT_H_MEM is followed by the block's guest pc: that is where its helper
    learns the pc of a slow access (plus four times the index in its data).
*/
static inline void psx_jit_emit_trampolines(psx_jit_ctx_t *c)
{
    psx_emit_t *const e = c->e;

    uint32_t done = 0; /* slots with a trampoline, one bit per word of the table */

    for (uint32_t i = 0; (i < c->call_count) && !e->overflow; i++)
    {
        const uint32_t slot = c->call_slot[i];

        if (done & (1u << (slot >> 2)))
            continue;

        done |= 1u << (slot >> 2);

        if (slot == PSX_JIT_H_MEM)
        {
            /* the pc word behind it must be aligned */
            if (psx_emit_here(e) & 3u)
                psx_emit16(e, 0xbf00u);
        }

        const uint32_t tramp = psx_emit_here(e);

        psx_emit_ldr_pc(e, PSX_JIT_HELPERS, slot);

        if (slot == PSX_JIT_H_MEM)
            psx_emit_data32(e, c->block_pc);

        for (uint32_t k = i; k < c->call_count; k++)
            if (c->call_slot[k] == slot)
                psx_jit_patch_bl(c->call_site[k], tramp);
    }
}

static inline int psx_jit_build_block_once(psx_jit_ctx_t *c, uint32_t pc, uint32_t max_instr,
                                           uint32_t page_mask, psx_jit_block_info_t *info)
{
    psx_emit_t *const e = c->e;

    psx_jit_stub_t stubs[PSX_JIT_MAX_STUBS];
    uint16_t *call_site[PSX_JIT_MAX_CALLS];
    uint8_t call_slot[PSX_JIT_MAX_CALLS];

    if (max_instr > PSX_JIT_BLOCK_MAX_INSTR)
        max_instr = PSX_JIT_BLOCK_MAX_INSTR;

    c->stubs = stubs;
    c->stub_count = 0;
    c->call_site = call_site;
    c->call_slot = call_slot;
    c->call_count = 0;
    c->block_pc = pc;
    c->force_next = 0;
    c->cycles_done = 0;
    c->exit_mode = PSX_JIT_EXIT_PC;
    c->in_delay = 0;
    c->cycles = 0;
    c->const_mask = 0;

    /* the budget test every block starts with: the flags are those of the
       SUBS that took the previous block's cycles off; the dispatcher enters
       behind it */
    psx_emit_it(e, PSX_CC_LE);
    psx_emit_ldr_pc(e, PSX_JIT_HELPERS, PSX_JIT_H_EXIT_LINK);

    uint32_t guest = pc;
    uint32_t count = 0;
    uint32_t native = 0;

    /* The dispatcher guarantees there is no pending load and no pending branch
       when a block is entered - and a block only hands over to the next one
       where that holds as well - so the first instruction needs no special
       care. */
    int force_interp = 0;

    /* Natively translated instructions do not advance pc / next_pc - and a block
       that was jumped into from another one starts without them as well: a
       chained jump publishes nothing. So an interpreted first instruction is
       handed its pc like any other; at the start of a block that is always
       right, since a block is never entered with a branch pending. */
    int state_stale = 1;

    int ended = 0;

    const uint32_t page_end = (pc | page_mask) + 1u;

    uint32_t op = c->read32(c->ud, guest);

    while ((count < max_instr) && (guest < page_end) && !e->overflow)
    {
        const uint32_t next_op = ((guest + 4u) < page_end)
                                     ? c->read32(c->ud, guest + 4u)
                                     : 0xffffffffu; /* unknown: treated as "uses everything" */

        c->guest = guest;
        c->next_op = next_op;

        /* The interpreter fires the BIOS TTY hook when it fetches from 0xb4 -
           the B function vector, which games call all the time - so native code
           calls it in front of the instruction there. The call is taken back
           when the instruction ends up interpreted: that fires it itself. (A
           branch whose delay slot is at 0xb4 calls it before its delay slot.) */
        const int hook_here = PSX_JIT_IS_BHOOK_PC(guest) && !force_interp;
        uint16_t *const hook_mark = e->cur;
        const uint32_t hook_calls = c->call_count;

        if (hook_here)
        {
            psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
            psx_jit_emit_call(c, PSX_JIT_H_BHOOK);
        }

        int emitted = 0;

        if (!force_interp)
        {
            emitted = psx_jit_translate_alu_c(c, op);

            if (emitted == 2)
            {
                /* LUI + ORI / ADDIU made one constant: two instructions */
                c->cycles += 4u;
                native += 2u;
                state_stale = 1;
                guest += 8u;
                count += 2u;

                if ((count < max_instr) && (guest < page_end))
                    op = c->read32(c->ud, guest);

                continue;
            }

            if (!emitted)
                emitted = psx_jit_translate_mem(c, op);

            if (!emitted)
                emitted = psx_jit_translate_trap_alu(c, op);

            if (!emitted)
                emitted = psx_jit_translate_div(c, op);

            if (!emitted)
                emitted = psx_jit_translate_cop2(c, op);

            if (!emitted)
                emitted = psx_jit_translate_gte_mem(c, op);

            if (!emitted)
                emitted = psx_jit_translate_cop0(c, op);

            /* A branch is translated together with its delay slot and always
               ends the block: the delay slot may go one past the size limit
               (which only bounds how long a block runs), not past the page. */
            if (!emitted && ((count + 1u) <= max_instr) && ((guest + 4u) < page_end))
            {
                emitted = psx_jit_translate_branch(c, op, next_op);
                ended = emitted;
            }
        }

        if (!emitted && hook_here)
        {
            e->cur = hook_mark;
            c->call_count = hook_calls;
        }

        if (ended)
        {
            /* branch plus delay slot: two instructions, cycles counted there */
            native += 2u;
            guest += 8u;
            count += 2u;

            break;
        }

        if (emitted)
        {
            /* a GTE command takes its own, variable, cycle count off r5 */
            if (!c->cycles_done)
                c->cycles += 2u;

            native++;
            force_interp = c->force_next;
            state_stale = 1;
            c->force_next = 0;
            c->cycles_done = 0;
        }
        else
        {
            /* The interpreter fetches from cpu->pc, so it is handed the pc when
               the natively translated instructions left that behind. It must
               not be touched when the previous instruction was interpreted: it
               may have set up a branch (pc = delay slot, next_pc = target). */
            psx_jit_emit_interp_one(c, state_stale);

            /* whatever it wrote is not known any more */
            psx_jit_kill_written(c, op);

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

    if (ended)
    {
        /* r3 says where to: the next block's link, a register target, or just
           the pc */
        if (c->exit_mode == PSX_JIT_EXIT_LINK)
        {
            psx_jit_emit_chain(c);
        }
        else if (c->exit_mode == PSX_JIT_EXIT_JR)
        {
            psx_jit_emit_jr(c);
        }
        else
        {
            psx_jit_flush_cycles(c);
            psx_jit_commit_pc_reg(e);
            psx_emit_bx(e, PSX_JIT_EXIT);
        }
    }
    else if (state_stale)
    {
        /* Straight line end (block size or page boundary): what follows is
           known, so this hands over as well - unless the next instruction has to
           see a pending load, which only the dispatcher arranges. */
        const uint32_t link = (!force_interp && c->get_link) ? c->get_link(c->ud, guest) : 0u;

        if (link)
        {
            psx_jit_r3_const(e, link - 1u, 1);
            psx_jit_emit_chain(c);
        }
        else
        {
            psx_jit_flush_cycles(c);
            psx_jit_publish_pc(e, guest);
            psx_emit_bx(e, PSX_JIT_EXIT);
        }
    }
    else
    {
        /* the last instruction was interpreted: it left pc / next_pc correct,
           branch targets included */
        psx_jit_flush_cycles(c);
        psx_emit_bx(e, PSX_JIT_EXIT);
    }

    psx_jit_emit_stubs(c);
    psx_jit_emit_trampolines(c);

    info->instr = count;
    info->native = native;

    return !e->overflow && (count != 0u);
}

/*
    Translates the block starting at pc into c->e. Translation stops at the end
    of the guest page (page_mask is the page size minus one), so a block is
    always built from a single page and one store can only ever invalidate the
    blocks of the page it hits.

    c->e, read32, get_link and ud have to be set up by the caller. Returns non
    zero when the block is usable.
*/
static inline int psx_jit_build_block(psx_jit_ctx_t *c, uint32_t pc, uint32_t max_instr,
                                      uint32_t page_mask, psx_jit_block_info_t *info)
{
    uint16_t *const start = c->e->cur;

    c->wide_sites = 0;
    c->range_fail = 0;

    int ok = psx_jit_build_block_once(c, pc, max_instr, page_mask, info);

    if (!ok && c->range_fail)
    {
        /* a stub too far for a 16 bit branch: the same block with wide ones
           (building it again has no effect beyond the code: the links it asked
           for exist already) */
        c->e->cur = start;
        c->e->overflow = 0;
        c->wide_sites = 1;
        c->range_fail = 0;

        ok = psx_jit_build_block_once(c, pc, max_instr, page_mask, info);
    }

    return ok;
}

#ifdef __cplusplus
}
#endif

#endif
