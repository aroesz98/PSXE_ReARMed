#ifndef PSX_JIT_BLOCK_H
#define PSX_JIT_BLOCK_H

/*
    Builds one block: the translation of a straight run of guest instructions,
    up to and including the first branch and its delay slot.

    This is everything that decides what a block looks like, kept free of the
    code cache and of the hardware so that the same code can be built on a host
    and its output run under an ARM emulator against a reference interpreter.
*/

#include <stdint.h>

#include "jit_emit.h"
#include "jit_translate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Instructions per block, see PSX_JIT_MAX_INSTR in jit.h */
#define PSX_JIT_BLOCK_MAX_INSTR 32u

typedef struct
{
    uint32_t instr;  /* guest instructions the block covers */
    uint32_t native; /* how many of them were translated natively */
} psx_jit_block_info_t;

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
    psx_emit_t *const e = c->e;

    uint16_t *exits[PSX_JIT_BLOCK_MAX_INSTR * 2u + 4u];
    uint32_t exit_count = 0;

    if (max_instr > PSX_JIT_BLOCK_MAX_INSTR)
        max_instr = PSX_JIT_BLOCK_MAX_INSTR;

    c->exits = exits;
    c->exit_count = &exit_count;
    c->exit_max = (uint32_t)(sizeof(exits) / sizeof(exits[0]));
    c->force_next = 0;
    c->cycles_done = 0;
    c->exit_mode = PSX_JIT_EXIT_PC;

    uint32_t guest = pc;
    uint32_t count = 0;
    uint32_t native = 0;
    uint32_t pending_cycles = 0;

    /* The dispatcher guarantees there is no pending load and no pending branch
       when a block is entered - and a block only hands over to the next one
       where that holds as well - so the first instruction needs no special
       care. */
    int force_interp = 0;

    /* Natively translated instructions do not advance pc / next_pc */
    int state_stale = 0;

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

        /* The interpreter fires the BIOS TTY hook when it fetches from 0xb4, so
           that instruction - and the branch it may be the delay slot of - stays
           interpreted. Decided at compile time, so it costs nothing to run. */
        if ((((guest & 0x3fffffffu) == 0x000000b4u)) ||
            (((guest + 4u) & 0x3fffffffu) == 0x000000b4u))
            force_interp = 1;

        int emitted = 0;

        if (!force_interp)
        {
            emitted = psx_jit_translate_alu(e, op);

            if (!emitted)
            {
                /* Anything that can escape to the interpreter needs the cycle
                   counter flushed first: the interpreter adds its own. */
                if (pending_cycles)
                {
                    psx_emit_add_imm12(e, PSX_JIT_CYC, PSX_JIT_CYC, pending_cycles);
                    pending_cycles = 0;
                }

                emitted = psx_jit_translate_mem(c, op);

                if (!emitted)
                    emitted = psx_jit_translate_trap_alu(c, op);

                if (!emitted)
                    emitted = psx_jit_translate_div(c, op);

                if (!emitted)
                    emitted = psx_jit_translate_cop2(c, op);

                if (!emitted)
                    emitted = psx_jit_translate_gte_mem(c, op);
            }

            /* A branch is translated together with its delay slot and always
               ends the block, so both instructions have to fit in it. */
            if (!emitted && ((count + 2u) <= max_instr) && ((guest + 4u) < page_end))
            {
                emitted = psx_jit_translate_branch(c, op, next_op);
                ended = emitted;
            }
        }

        if (ended)
        {
            /* branch plus delay slot: two instructions */
            pending_cycles += 4u;
            native += 2u;
            guest += 8u;
            count += 2u;

            break;
        }

        if (emitted)
        {
            /* a GTE command adds its own, variable, cycle count */
            if (!c->cycles_done)
                pending_cycles += 2u;

            native++;
            force_interp = c->force_next;
            state_stale = 1;
            c->force_next = 0;
            c->cycles_done = 0;
        }
        else
        {
            if (pending_cycles)
            {
                psx_emit_add_imm12(e, PSX_JIT_CYC, PSX_JIT_CYC, pending_cycles);
                pending_cycles = 0;
            }

            /* The interpreter fetches from cpu->pc, so it is handed the pc when
               the natively translated instructions left that behind. It must
               not be touched when the previous instruction was interpreted: it
               may have set up a branch (pc = delay slot, next_pc = target). */
            psx_jit_emit_interp_one(c, state_stale);

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
        psx_emit_add_imm12(e, PSX_JIT_CYC, PSX_JIT_CYC, pending_cycles);

    if (ended)
    {
        /* r3 says where to: the next block's link, or just the pc */
        if (c->exit_mode == PSX_JIT_EXIT_LINK)
            psx_jit_emit_chain(e);
        else
            psx_jit_commit_pc_reg(e);
    }
    else if (state_stale)
    {
        /* Straight line end (block size or page boundary): what follows is
           known, so this hands over as well - unless the next instruction has to
           see a pending load, which only the dispatcher arranges. Nothing to do
           when the last instruction was interpreted: it left pc / next_pc
           correct, branch targets included. */
        const uint32_t link = (!force_interp && c->get_link) ? c->get_link(c->ud, guest) : 0u;

        if (link)
        {
            psx_emit_imm32(e, PSX_R3, link);
            psx_jit_emit_chain(e);
        }
        else
        {
            psx_jit_publish_pc(e, guest);
        }
    }

    /* exits from the middle land here: pc is already correct */
    const uint32_t out = psx_emit_here(e);

    for (uint32_t i = 0; i < exit_count; i++)
        psx_emit_patch_bcond(exits[i], PSX_CC_NE, out);

    psx_emit_bx(e, PSX_JIT_EXIT);

    info->instr = count;
    info->native = native;

    return !e->overflow && (count != 0u);
}

#ifdef __cplusplus
}
#endif

#endif
