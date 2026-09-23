#ifndef PSX_JIT_TRANSLATE_H
#define PSX_JIT_TRANSLATE_H

/*
    Guest instruction -> Thumb-2 translation.

    Anything that can trap, that changes control flow, or whose semantics depend
    on state the translator does not track is handed to the interpreter - one
    single instruction at a time, from inside the same block. That is what keeps
    behaviour identical to the pure interpreter while the hot arithmetic and the
    RAM accesses run as native code.

    The code layout follows the recompiler of DuckStation (a block is a hot path
    that runs straight through, everything that is not taken normally lives in
    "far code" behind it, constants are known at compile time), written anew for
    Thumb-2 and the Cortex-M7: nothing of DuckStation's code is used, its ARM
    backend emits A32 code and relies on an MMU.

    Host register usage inside a block:
      r0..r3, r12  scratch / helper arguments (r3: where the block goes next)
      r4      psx_cpu_t *
      r5      emulated cycles left in the slice, counting down. A block takes its
              cycles off right before it hands over, and the block it hands over
              to begins with the test of those flags: <= 0 leaves.
      r6      base of the guest RAM buffer
      r7      base of the "this part of RAM holds translated code" table
      r8      base of the link table: a block jumps to the next one through
              [r8 + link offset]
      r9      table of helper entry points
      r10     where a block returns to (the dispatcher's entry stub)
      r11     keeps r3 across a helper call inside a branch delay slot

    A block has no prologue or epilogue of its own - the entry stub sets the
    registers up once and every block leaves through r10 - and it holds no
    address that depends on where it sits: branches stay inside the block,
    helpers are called through r9 and other blocks are reached through their
    link. That makes a block a plain run of bytes that can be copied between the
    code tiers.

    Helpers are reached with a BL to a trampoline at the end of the block (one
    LDR.W PC through r9 per helper the block uses), so a call is four bytes and
    stays right when the block is copied. Slow paths are "stubs" behind the
    block: such a call followed by a word or two of data (a description of the
    access, the guest pc) that the helper reads through its return address. A
    helper that has to leave the block does so itself, so the block does not
    test anything after a call.
*/

#include <stdint.h>
#include <stddef.h>

#include "jit_emit.h"
#include "../cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_JIT_CPU PSX_R4
#define PSX_JIT_CYC PSX_R5
#define PSX_JIT_RAM PSX_R6
#define PSX_JIT_PAGES PSX_R7
#define PSX_JIT_LINKS PSX_R8
#define PSX_JIT_HELPERS PSX_R9
#define PSX_JIT_EXIT PSX_R10
#define PSX_JIT_KEEP PSX_R11

/* slots of the helper table, as byte offsets from r9 */
#define PSX_JIT_H_INTERP 0u     /* stub:               run one instruction, pc already right */
#define PSX_JIT_H_INTERP_AT 4u  /* stub + pc:          ... after publishing pc               */
#define PSX_JIT_H_LOAD_AT 8u    /* stub + pc:          ... and apply the load                */
#define PSX_JIT_H_DELAY 12u     /* stub + pc, flags:   run a branch delay slot, never returns */
#define PSX_JIT_H_GTE 16u       /* int32_t f(cpu, opcode)       GTE command                  */
#define PSX_JIT_H_GTE_READ 20u  /* uint32_t f(cpu, reg)         MFC2 / CFC2                  */
#define PSX_JIT_H_GTE_WRITE 24u /* void f(cpu, reg, value)      MTC2 / CTC2                  */
#define PSX_JIT_H_GTE_MEM 28u   /* stub + pc, opcode:  LWC2 / SWC2                           */
#define PSX_JIT_H_MEM 32u       /* stub + pc, desc:    access that is not plain RAM, r0 addr */
#define PSX_JIT_H_EXIT_LINK 36u /* not called: jumped to, with a link offset in r3, to leave */
#define PSX_JIT_H_JR 40u        /* not called: jumped to with a guest pc in r3              */
#define PSX_JIT_H_PC_BASE 44u   /* not a function: the guest pc of every link               */
#define PSX_JIT_H_SPAD 48u      /* not a function: the scratchpad buffer                     */
#define PSX_JIT_H_JRC 52u       /* not a function: the register jump target cache           */
#define PSX_JIT_H_BHOOK 56u     /* void f(cpu)                  the BIOS TTY hook at 0xb4    */
#define PSX_JIT_H_COUNT 15u

/* the address the interpreter fires the BIOS TTY hook at (the B function vector) */
#define PSX_JIT_IS_BHOOK_PC(pc) ((((pc) & 0x3fffffffu)) == 0x000000b4u)

/*
    The one data word of a PSX_JIT_H_MEM stub. The helper serves the access - the
    scratchpad right in the stub, everything else through the bus - and goes
    back to the block "back" halfwords before the end of the word, with a loaded
    value in r2. The guest pc is the block's (kept next to the trampoline the
    stub calls) plus four times the instruction's index.
*/
#define PSX_JIT_MD_RT(d) ((d) & 0x1fu)
#define PSX_JIT_MD_SIZE(d) (1u << (((d) >> 5) & 3u))
#define PSX_JIT_MD_SIGNED 0x080u
#define PSX_JIT_MD_STORE 0x100u
#define PSX_JIT_MD_INDEX(d) (((d) >> 9) & 0x1fu)
#define PSX_JIT_MD_DELAY 0x4000u  /* in a branch delay slot: r3 says where the branch goes */
#define PSX_JIT_MD_LINK 0x8000u   /* ... and r3 is a link offset rather than a pc         */
#define PSX_JIT_MD_CYCLES(d) (((d) >> 16) & 0x3fu) /* not yet taken off r5 */
#define PSX_JIT_MD_BACK(d) ((d) >> 22)

/* flags word of a PSX_JIT_H_DELAY stub */
#define PSX_JIT_DF_LINK 1u

/*
    A link is how blocks refer to each other: the entry point of the block at a
    guest address, one word. It exists from the moment some block branches to
    that address, before anything has been compiled for it - until then (and
    again after the block has been invalidated) it points at the dispatcher's
    "leave through a link" stub, so jumping through a link is always safe. Moving
    a block between the code tiers only has to update its one link.

    Blocks name a link by its offset in the table (r8), which a MOVW holds; the
    guest pc of a link is kept at the same offset in a table of its own (see
    PSX_JIT_H_PC_BASE): only leaving needs it.
*/
typedef struct
{
    uint32_t code; /* host entry point (Thumb) */
} psx_jit_link_t;

/* what a block ending branch leaves in r3 */
#define PSX_JIT_EXIT_PC 0   /* the guest pc: the block returns to the dispatcher */
#define PSX_JIT_EXIT_LINK 1 /* the link offset of the next block: it may be run directly */
#define PSX_JIT_EXIT_JR 2   /* a guest pc from a register: looked up without the dispatcher */

/* Every block begins with the budget test: IT LE + LDR.W PC. The dispatcher
   enters behind it. */
#define PSX_JIT_ENTRY_TEST_BYTES 6u

/* Guest RAM window test: bits 28:21 are zero exactly for the three RAM mirrors
   (KUSEG / KSEG0 / KSEG1, low 2 MB). Scratchpad, I/O, BIOS and the cache
   control register all fail it and take the slow path. */
#define PSX_JIT_RAM_WINDOW_MASK 0x1fe00000u

/* offsets inside psx_cpu_t */
#define PSX_JIT_OFF_R(n) ((uint32_t)(offsetof(psx_cpu_t, r) + (n) * 4u))
#define PSX_JIT_OFF_PC ((uint32_t)offsetof(psx_cpu_t, pc))
#define PSX_JIT_OFF_NEXT_PC ((uint32_t)offsetof(psx_cpu_t, next_pc))
#define PSX_JIT_OFF_HI ((uint32_t)offsetof(psx_cpu_t, hi))
#define PSX_JIT_OFF_LO ((uint32_t)offsetof(psx_cpu_t, lo))
#define PSX_JIT_OFF_LOAD_D ((uint32_t)offsetof(psx_cpu_t, load_d))
#define PSX_JIT_OFF_LOAD_V ((uint32_t)offsetof(psx_cpu_t, load_v))
#define PSX_JIT_OFF_COP0(n) ((uint32_t)(offsetof(psx_cpu_t, cop0_r) + (n) * 4u))

/* guest instruction fields */
#define PSX_OP(op) ((op) >> 26)
#define PSX_RS(op) (((op) >> 21) & 0x1fu)
#define PSX_RT(op) (((op) >> 16) & 0x1fu)
#define PSX_RD(op) (((op) >> 11) & 0x1fu)
#define PSX_SA(op) (((op) >> 6) & 0x1fu)
#define PSX_FN(op) ((op) & 0x3fu)
#define PSX_IMM(op) ((op) & 0xffffu)
#define PSX_SIMM(op) ((uint32_t)(int32_t)(int16_t)((op) & 0xffffu))

/* One flag per 256 bytes of guest RAM says whether translated code came from
   there (g_psx_jit_code_pages; the same number is in bus_fast.h). It used to be
   one per 1 KB page, and in an FF7 battle some 100 000 stores a second went the
   long way round only because their variables share a page with code. */
#define PSX_JIT_CODE_FLAG_SHIFT 8u

/* ------------------------------------------------------------------ stubs */

#define PSX_JIT_STUB_MEM 0u     /* H_MEM: access that is not plain RAM              */
#define PSX_JIT_STUB_TRAP 1u    /* H_INTERP_AT, then leave: the instruction traps   */
#define PSX_JIT_STUB_DELAY 2u   /* H_DELAY: the delay slot traps, never comes back  */
#define PSX_JIT_STUB_GTE_MEM 3u /* H_GTE_MEM: LWC2 / SWC2 the long way              */
#define PSX_JIT_STUB_LEAVE 4u   /* the instruction ran: leave the block behind it   */

typedef struct
{
    uint16_t *site[3]; /* B<cond>.W in the block that lead here, NULL: none */
    uint16_t *resume;  /* where the stub goes back to                        */
    uint8_t cond[3];
    uint8_t kind;
    uint8_t set_addr;  /* the stub puts "addr" into r0 first                 */
    uint8_t narrow;    /* the sites are the 16 bit branches                  */
    uint32_t pc;       /* guest pc of the instruction                        */
    uint32_t data;     /* second data word: desc / flags / opcode            */
    uint32_t cycles;   /* native cycles of the block not yet taken off r5    */
    uint32_t addr;
} psx_jit_stub_t;

#define PSX_JIT_MAX_STUBS 48u

/* helper calls of one block, each a BL to be pointed at its trampoline */
#define PSX_JIT_MAX_CALLS 64u

/* Translation context for one instruction */
typedef struct
{
    psx_emit_t *e;
    uint32_t guest;         /* address of the instruction being translated */
    uint32_t next_op;       /* the instruction that follows it             */
    int force_next;         /* the next instruction must be interpreted    */
    int cycles_done;        /* the instruction accounted for its own cycles*/
    int exit_mode;          /* PSX_JIT_EXIT_*: what the ending branch left in r3 */
    int in_delay;           /* translating a branch delay slot             */

    /* native cycles of the block that r5 does not know about yet */
    uint32_t cycles;

    /* guest registers whose value is known at this point of the block */
    uint32_t const_mask;
    uint32_t const_val[32];

    psx_jit_stub_t *stubs;
    uint32_t stub_count;

    uint16_t **call_site;
    uint8_t *call_slot;
    uint32_t call_count;

    uint32_t block_pc;      /* guest address of the block's first instruction */

    /* Branches to the stubs are the 16 bit forms, which reach 254 bytes ahead;
       a block with a stub out of their reach is built again with the wide ones. */
    int wide_sites;
    int range_fail;

    /* guest code, to look at the instructions a branch leads to */
    uint32_t (*read32)(void *ud, uint32_t addr);

    /* link offset + 1 of the block at pc, 0 when none can be had */
    uint32_t (*get_link)(void *ud, uint32_t pc);

    void *ud;
} psx_jit_ctx_t;

static inline psx_jit_stub_t *psx_jit_new_stub(psx_jit_ctx_t *c, uint32_t kind)
{
    if (c->stub_count >= PSX_JIT_MAX_STUBS)
    {
        c->e->overflow = 1; /* the block is given up and built shorter */
        return &c->stubs[0];
    }

    psx_jit_stub_t *const s = &c->stubs[c->stub_count++];

    s->site[0] = NULL;
    s->site[1] = NULL;
    s->site[2] = NULL;
    s->resume = NULL;
    s->cond[0] = PSX_CC_NE;
    s->cond[1] = PSX_CC_NE;
    s->cond[2] = PSX_CC_NE;
    s->kind = (uint8_t)kind;
    s->set_addr = 0;
    s->narrow = c->wide_sites ? 0u : 1u;
    s->pc = c->guest;
    s->data = 0;
    s->cycles = c->cycles;
    s->addr = 0;

    return s;
}

/* ------------------------------------------------------------------ constants */

static inline int psx_jit_is_const(const psx_jit_ctx_t *c, uint32_t r)
{
    return (r == 0) || (c->const_mask & (1u << r));
}

static inline uint32_t psx_jit_const_of(const psx_jit_ctx_t *c, uint32_t r)
{
    return r ? c->const_val[r] : 0u;
}

static inline void psx_jit_set_const(psx_jit_ctx_t *c, uint32_t r, uint32_t v)
{
    if (r)
    {
        c->const_mask |= 1u << r;
        c->const_val[r] = v;
    }
}

static inline void psx_jit_kill_const(psx_jit_ctx_t *c, uint32_t r)
{
    c->const_mask &= ~(1u << r);
}

/* a branch from the block to a stub, patched once the stubs are laid out */
static inline uint16_t *psx_jit_stub_site(psx_jit_ctx_t *c, uint32_t cond)
{
    if (cond == PSX_CC_AL)
        return c->wide_sites ? psx_emit_b_fwd(c->e) : psx_emit_b_short_fwd(c->e);

    return c->wide_sites ? psx_emit_bcond_fwd(c->e, cond) : psx_emit_bcond_short_fwd(c->e, cond);
}

/* ------------------------------------------------------------------ registers */

/* load a guest register into a host register (guest r0 is constant zero) */
static inline void psx_jit_ld_reg(psx_emit_t *e, uint32_t host, uint32_t guest)
{
    if (guest == 0)
        psx_emit_mov_imm8(e, host, 0);
    else
        psx_emit_ldr_imm(e, host, PSX_JIT_CPU, PSX_JIT_OFF_R(guest));
}

/* store a host register into a guest register (writes to guest r0 are dropped) */
static inline void psx_jit_st_reg(psx_emit_t *e, uint32_t host, uint32_t guest)
{
    if (guest != 0)
        psx_emit_str_imm(e, host, PSX_JIT_CPU, PSX_JIT_OFF_R(guest));
}

/* A guest register that gets a value known at compile time. Constants are
   always written to the register file as well - a helper or the interpreter
   may read it at any time - the translator only remembers them. */
static inline void psx_jit_st_const(psx_jit_ctx_t *c, uint32_t guest, uint32_t value)
{
    if (!guest)
        return;

    psx_emit_const(c->e, PSX_R0, value);
    psx_jit_st_reg(c->e, PSX_R0, guest);
    psx_jit_set_const(c, guest, value);
}

/* ------------------------------------------------------------------ calls */

/* Calls a helper. The helpers live in ITCM and OCRAM while a block may run
   from SDRAM, far outside BL range - so the BL goes to a trampoline at the end
   of the block, LDR.W PC, [r9, #slot], which moves with the block. */
static inline void psx_jit_emit_call(psx_jit_ctx_t *c, uint32_t slot)
{
    if (c->call_count >= PSX_JIT_MAX_CALLS)
    {
        c->e->overflow = 1;
        return;
    }

    c->call_site[c->call_count] = c->e->cur;
    c->call_slot[c->call_count] = (uint8_t)slot;
    c->call_count++;

    psx_emit32(c->e, 0xf000u, 0xd000u); /* BL, pointed at the trampoline later */
}

/* Takes the block's native cycles so far off r5 - before anything that counts
   its own (the interpreter) or may leave the block. */
static inline void psx_jit_flush_cycles(psx_jit_ctx_t *c)
{
    if (c->cycles)
    {
        psx_emit_subs_imm(c->e, PSX_JIT_CYC, PSX_JIT_CYC, c->cycles);
        c->cycles = 0;
    }
}

/* A call with data words behind it: pads so that the data is word aligned
   (the helpers read it with plain loads, and a block is only ever copied to
   word aligned places). */
static inline void psx_jit_emit_stub_call(psx_jit_ctx_t *c, uint32_t slot)
{
    if (psx_emit_here(c->e) & 3u)
        psx_emit16(c->e, 0xbf00u); /* NOP */

    psx_jit_emit_call(c, slot);
}

/* "Run this one instruction in the interpreter." The helper leaves the block
   by itself when control flow diverged or when the instruction produced state
   only the dispatcher deals with. publish_pc: the natively translated code
   before it did not advance pc / next_pc, so the helper sets them. */
static inline void psx_jit_emit_interp_call(psx_jit_ctx_t *c, int publish_pc, int is_load)
{
    psx_jit_flush_cycles(c);

    if (publish_pc)
    {
        psx_jit_emit_stub_call(c, is_load ? PSX_JIT_H_LOAD_AT : PSX_JIT_H_INTERP_AT);
        psx_emit_data32(c->e, c->guest);
    }
    else
    {
        psx_jit_emit_call(c, PSX_JIT_H_INTERP);
    }
}

static inline void psx_jit_emit_interp_one(psx_jit_ctx_t *c, int publish_pc)
{
    psx_jit_emit_interp_call(c, publish_pc, 0);
}

/* Publishes the guest pc so the interpreter / the dispatcher can take over at
   this instruction */
static inline void psx_jit_publish_pc(psx_emit_t *e, uint32_t guest)
{
    psx_emit_const(e, PSX_R0, guest);
    psx_emit_add_imm12(e, PSX_R1, PSX_R0, 4);

    /* pc and next_pc are adjacent, so a single store covers both */
    psx_emit_strd_imm(e, PSX_R0, PSX_R1, PSX_JIT_CPU, PSX_JIT_OFF_PC);
}

/* Writes the guest pc that the block leaves behind: r3 holds it, next_pc is
   always one instruction further. */
static inline void psx_jit_commit_pc_reg(psx_emit_t *e)
{
    psx_emit_add_imm12(e, PSX_R0, PSX_R3, 4);
    psx_emit_strd_imm(e, PSX_R3, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_PC);
}

_Static_assert((offsetof(psx_cpu_t, next_pc) == offsetof(psx_cpu_t, pc) + 4u) && ((offsetof(psx_cpu_t, pc) & 3u) == 0u),
               "pc / next_pc are written with one STRD");
_Static_assert((offsetof(psx_cpu_t, lo) == offsetof(psx_cpu_t, hi) + 4u) && ((offsetof(psx_cpu_t, hi) & 3u) == 0u),
               "hi / lo are written with one STRD");
_Static_assert((offsetof(psx_cpu_t, load_v) == offsetof(psx_cpu_t, load_d) + 4u) &&
                   ((offsetof(psx_cpu_t, load_d) & 3u) == 0u),
               "load_d / load_v are written with one STRD");

/* The end of a block whose r3 holds the link offset of the next one: the cycles
   come off r5, and the next block tests the flags that leaves. */
static inline void psx_jit_emit_chain(psx_jit_ctx_t *c)
{
    if (c->cycles)
        psx_emit_subs_imm(c->e, PSX_JIT_CYC, PSX_JIT_CYC, c->cycles);
    else
        psx_emit_cmp_imm8(c->e, PSX_JIT_CYC, 0);

    c->cycles = 0;

    psx_emit_ldr_reg(c->e, PSX_PC, PSX_JIT_LINKS, PSX_R3);
}

/* The same through the register jump helper: r3 is a guest pc */
static inline void psx_jit_emit_jr(psx_jit_ctx_t *c)
{
    psx_jit_flush_cycles(c);
    psx_emit_ldr_pc(c->e, PSX_JIT_HELPERS, PSX_JIT_H_JR);
}

/* flags in the low bits of the pc word of a PSX_JIT_H_GTE_MEM stub */
#define PSX_JIT_GF_DELAY 1u /* a branch delay slot: r3 says where the branch goes */
#define PSX_JIT_GF_LINK 2u  /* ... as a link offset                               */

/* Loads a constant into r3 with a fixed length sequence, so it can sit inside
   an IT block: a link offset is always one MOVW, a pc two. */
static inline void psx_jit_r3_const(psx_emit_t *e, uint32_t value, int is_link)
{
    psx_emit_movw(e, PSX_R3, value & 0xffffu);

    if (!is_link)
        psx_emit_movt(e, PSX_R3, value >> 16);
}

/* Does this instruction read guest register r? Conservative. */
static inline int psx_jit_reads_reg(uint32_t op, uint32_t r)
{
    if (r == 0)
        return 0;

    const uint32_t o = PSX_OP(op);

    if (o == 0x00)
        return (PSX_RS(op) == r) || (PSX_RT(op) == r);

    if ((o == 0x02) || (o == 0x03) || (o == 0x0f))
        return 0;

    /* Branches. They mattered most of all: "LW ; BEQ" is how every loop over
       a list ends, and with branches counted as "reads everything" each such
       load was left pending, which sent the branch and its delay slot through
       the interpreter and the block back to the dispatcher. */
    if ((o == 0x04) || (o == 0x05)) /* BEQ / BNE */
        return (PSX_RS(op) == r) || (PSX_RT(op) == r);

    if ((o == 0x01) || (o == 0x06) || (o == 0x07)) /* REGIMM, BLEZ, BGTZ */
        return PSX_RS(op) == r;

    if ((o >= 0x08) && (o <= 0x0e))
        return PSX_RS(op) == r;

    if ((o >= 0x20) && (o <= 0x26))
        return (PSX_RS(op) == r) || (((o == 0x22) || (o == 0x26)) && (PSX_RT(op) == r));

    if ((o >= 0x28) && (o <= 0x2e))
        return (PSX_RS(op) == r) || (PSX_RT(op) == r);

    /* COP2. These matter: 3D code is one load after another with GTE register
       moves in between, and treating those as "touches everything" sent every
       such load through the load delay slot and the move to the interpreter. */
    if (o == 0x12)
    {
        if (op & 0x02000000u)
            return 0; /* a GTE command works on GTE registers only */

        const uint32_t rs = PSX_RS(op);

        if ((rs == 0x04) || (rs == 0x06)) /* MTC2 / CTC2 */
            return PSX_RT(op) == r;

        if ((rs == 0x00) || (rs == 0x02)) /* MFC2 / CFC2 */
            return 0;

        return 1;
    }

    if ((o == 0x32) || (o == 0x3a)) /* LWC2 / SWC2: the base register */
        return PSX_RS(op) == r;

    return 1;
}

/* Does this instruction write guest register r? Conservative. */
static inline int psx_jit_writes_reg(uint32_t op, uint32_t r)
{
    if (r == 0)
        return 0;

    const uint32_t o = PSX_OP(op);

    if (o == 0x00)
    {
        const uint32_t fn = PSX_FN(op);

        if (fn == 0x09) /* JALR */
            return PSX_RD(op) == r;

        if ((fn == 0x18) || (fn == 0x19) || (fn == 0x1a) || (fn == 0x1b)) /* mult / div */
            return 0;

        if ((fn == 0x11) || (fn == 0x13)) /* MTHI / MTLO */
            return 0;

        if ((fn == 0x08) || (fn == 0x0c) || (fn == 0x0d)) /* JR, SYSCALL, BREAK */
            return 0;

        return PSX_RD(op) == r;
    }

    if ((o == 0x03) || (o == 0x01)) /* JAL, BLTZAL / BGEZAL */
        return r == 31u;

    if ((o == 0x02) || ((o >= 0x04) && (o <= 0x07))) /* J, BEQ, BNE, BLEZ, BGTZ */
        return 0;

    if ((o >= 0x08) && (o <= 0x0f))
        return PSX_RT(op) == r;

    if ((o >= 0x20) && (o <= 0x26))
        return PSX_RT(op) == r;

    if ((o >= 0x28) && (o <= 0x2e))
        return 0;

    if (o == 0x12)
    {
        if (op & 0x02000000u)
            return 0;

        const uint32_t rs = PSX_RS(op);

        if ((rs == 0x00) || (rs == 0x02)) /* MFC2 / CFC2 */
            return PSX_RT(op) == r;

        if ((rs == 0x04) || (rs == 0x06)) /* MTC2 / CTC2 */
            return 0;

        return 1;
    }

    if ((o == 0x32) || (o == 0x3a)) /* LWC2 / SWC2 */
        return 0;

    return 1;
}

/* Forgets what is known about every register this instruction may write */
static inline void psx_jit_kill_written(psx_jit_ctx_t *c, uint32_t op)
{
    if (!c->const_mask)
        return;

    for (uint32_t r = 1; r < 32u; r++)
        if ((c->const_mask & (1u << r)) && psx_jit_writes_reg(op, r))
            psx_jit_kill_const(c, r);
}

/* ------------------------------------------------------------------ ALU */

/* the result of an ALU instruction whose inputs are all known */
static inline int psx_jit_fold_alu(const psx_jit_ctx_t *c, uint32_t op, uint32_t *dst, uint32_t *value)
{
    const uint32_t rs = PSX_RS(op);
    const uint32_t rt = PSX_RT(op);
    const uint32_t o = PSX_OP(op);

    if (o == 0x0f) /* LUI */
    {
        *dst = rt;
        *value = PSX_IMM(op) << 16;
        return 1;
    }

    if ((o == 0x09) || (o == 0x0d) || (o == 0x0c) || (o == 0x0e)) /* ADDIU / ORI / ANDI / XORI */
    {
        if (!psx_jit_is_const(c, rs))
            return 0;

        const uint32_t s = psx_jit_const_of(c, rs);

        *dst = rt;
        *value = (o == 0x09) ? (s + PSX_SIMM(op))
                             : ((o == 0x0d) ? (s | PSX_IMM(op)) : ((o == 0x0c) ? (s & PSX_IMM(op)) : (s ^ PSX_IMM(op))));
        return 1;
    }

    if (o == 0x00)
    {
        const uint32_t fn = PSX_FN(op);

        if ((fn == 0x00) && psx_jit_is_const(c, rt)) /* SLL */
        {
            *dst = PSX_RD(op);
            *value = psx_jit_const_of(c, rt) << PSX_SA(op);
            return 1;
        }

        if (((fn == 0x21) || (fn == 0x25)) && psx_jit_is_const(c, rs) && psx_jit_is_const(c, rt)) /* ADDU / OR */
        {
            const uint32_t s = psx_jit_const_of(c, rs);
            const uint32_t t = psx_jit_const_of(c, rt);

            *dst = PSX_RD(op);
            *value = (fn == 0x21) ? (s + t) : (s | t);
            return 1;
        }
    }

    return 0;
}

static inline int psx_jit_translate_alu(psx_emit_t *e, uint32_t op)
{
    const uint32_t rs = PSX_RS(op);
    const uint32_t rt = PSX_RT(op);
    const uint32_t rd = PSX_RD(op);

    switch (PSX_OP(op))
    {
    case 0x00: /* SPECIAL */
        switch (PSX_FN(op))
        {
        case 0x00: /* SLL, also NOP */
        case 0x02: /* SRL */
        case 0x03: /* SRA */
        {
            if ((op == 0) || (rd == 0))
                return 1;

            const uint32_t type = (PSX_FN(op) == 0x00) ? 0u : ((PSX_FN(op) == 0x02) ? 1u : 2u);

            psx_jit_ld_reg(e, PSX_R0, rt);
            psx_emit_shift_imm(e, type, PSX_R0, PSX_R0, PSX_SA(op));
            psx_jit_st_reg(e, PSX_R0, rd);

            return 1;
        }

        case 0x04: /* SLLV */
        case 0x06: /* SRLV */
        case 0x07: /* SRAV */
        {
            if (rd == 0)
                return 1;

            const uint32_t type = (PSX_FN(op) == 0x04) ? 0u : ((PSX_FN(op) == 0x06) ? 1u : 2u);

            psx_jit_ld_reg(e, PSX_R0, rt);
            psx_jit_ld_reg(e, PSX_R1, rs);
            psx_emit_and_imm12(e, PSX_R1, PSX_R1, 31); /* MIPS shifts use rs & 31 */
            psx_emit_shift_reg(e, type, PSX_R0, PSX_R0, PSX_R1);
            psx_jit_st_reg(e, PSX_R0, rd);

            return 1;
        }

        case 0x10: /* MFHI */
        case 0x12: /* MFLO */
        {
            if (rd == 0)
                return 1;

            psx_emit_ldr_imm(e, PSX_R0, PSX_JIT_CPU,
                             (PSX_FN(op) == 0x10) ? PSX_JIT_OFF_HI : PSX_JIT_OFF_LO);
            psx_jit_st_reg(e, PSX_R0, rd);

            return 1;
        }

        case 0x11: /* MTHI */
        case 0x13: /* MTLO */
        {
            psx_jit_ld_reg(e, PSX_R0, rs);
            psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU,
                             (PSX_FN(op) == 0x11) ? PSX_JIT_OFF_HI : PSX_JIT_OFF_LO);

            return 1;
        }

        case 0x18: /* MULT */
        case 0x19: /* MULTU */
        {
            /* One host instruction each way, and no branch, so this is also
               allowed in a delay slot. hi and lo are adjacent. */
            psx_jit_ld_reg(e, PSX_R0, rs);
            psx_jit_ld_reg(e, PSX_R1, rt);

            /* r12 takes the high half: r3 may carry where the branch this
               instruction is the delay slot of goes. */
            if (PSX_FN(op) == 0x18)
                psx_emit_smull(e, PSX_R2, PSX_R12, PSX_R0, PSX_R1);
            else
                psx_emit_umull(e, PSX_R2, PSX_R12, PSX_R0, PSX_R1);

            psx_emit_strd_imm(e, PSX_R12, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_HI);

            return 1;
        }

        case 0x21: /* ADDU */
        case 0x23: /* SUBU */
        case 0x24: /* AND */
        case 0x25: /* OR */
        case 0x26: /* XOR */
        case 0x27: /* NOR */
        {
            if (rd == 0)
                return 1;

            /* the moves: `addu rd, rs, zero` and `or rd, zero, rt` */
            if (((PSX_FN(op) == 0x21) || (PSX_FN(op) == 0x25)) && ((rs == 0) || (rt == 0)))
            {
                psx_jit_ld_reg(e, PSX_R0, rs ? rs : rt);
                psx_jit_st_reg(e, PSX_R0, rd);
                return 1;
            }

            psx_jit_ld_reg(e, PSX_R0, rs);
            psx_jit_ld_reg(e, PSX_R1, rt);

            switch (PSX_FN(op))
            {
            case 0x21: psx_emit_add_reg(e, PSX_R0, PSX_R0, PSX_R1); break;
            case 0x23: psx_emit_sub_reg(e, PSX_R0, PSX_R0, PSX_R1); break;
            case 0x24: psx_emit_and_reg(e, PSX_R0, PSX_R0, PSX_R1); break;
            case 0x25: psx_emit_orr_reg(e, PSX_R0, PSX_R0, PSX_R1); break;
            case 0x26: psx_emit_eor_reg(e, PSX_R0, PSX_R0, PSX_R1); break;
            default:
                psx_emit_orr_reg(e, PSX_R0, PSX_R0, PSX_R1);
                psx_emit_mvn_reg(e, PSX_R0, PSX_R0);
                break;
            }

            psx_jit_st_reg(e, PSX_R0, rd);

            return 1;
        }

        case 0x2a: /* SLT */
        case 0x2b: /* SLTU */
        {
            if (rd == 0)
                return 1;

            psx_jit_ld_reg(e, PSX_R0, rs);
            psx_jit_ld_reg(e, PSX_R1, rt);
            psx_emit_mov_imm8(e, PSX_R2, 0);
            psx_emit_cmp_reg(e, PSX_R0, PSX_R1);
            psx_emit_it(e, (PSX_FN(op) == 0x2a) ? PSX_CC_LT : PSX_CC_CC);
            psx_emit_mov_imm8(e, PSX_R2, 1);
            psx_jit_st_reg(e, PSX_R2, rd);

            return 1;
        }

        default:
            return 0;
        }

    case 0x09: /* ADDIU */
    case 0x0a: /* SLTI */
    case 0x0b: /* SLTIU */
    case 0x0c: /* ANDI */
    case 0x0d: /* ORI */
    case 0x0e: /* XORI */
    {
        if (rt == 0)
            return 1;

        const uint32_t o = PSX_OP(op);
        const uint32_t imm = (o >= 0x0c) ? PSX_IMM(op) : PSX_SIMM(op);
        const uint32_t imm12 = psx_thumb_expand_imm(imm);

        psx_jit_ld_reg(e, PSX_R0, rs);

        /* ADDIU is the most common instruction there is, so the constant goes
           into the instruction itself whenever it fits. */
        if (o == 0x09)
        {
            if (imm == 0u)
                ;
            else if (imm < 0x1000u)
                psx_emit_add_imm12(e, PSX_R0, PSX_R0, imm);
            else if ((uint32_t)(0u - imm) < 0x1000u)
                psx_emit_sub_imm12(e, PSX_R0, PSX_R0, 0u - imm);
            else
            {
                psx_emit_imm32(e, PSX_R1, imm);
                psx_emit_add_reg(e, PSX_R0, PSX_R0, PSX_R1);
            }

            psx_jit_st_reg(e, PSX_R0, rt);

            return 1;
        }

        if ((o >= 0x0c) && (o <= 0x0e) && (imm12 != 0xffffffffu))
        {
            if (o == 0x0c)
                psx_emit_and_imm12(e, PSX_R0, PSX_R0, imm12);
            else if (o == 0x0d)
                psx_emit_orr_imm12(e, PSX_R0, PSX_R0, imm12);
            else
                psx_emit_eor_imm12(e, PSX_R0, PSX_R0, imm12);

            psx_jit_st_reg(e, PSX_R0, rt);

            return 1;
        }

        if (((o == 0x0a) || (o == 0x0b)) && (imm12 != 0xffffffffu))
        {
            psx_emit_mov_imm8(e, PSX_R2, 0);
            psx_emit_cmp_imm12(e, PSX_R0, imm12);
            psx_emit_it(e, (o == 0x0a) ? PSX_CC_LT : PSX_CC_CC);
            psx_emit_mov_imm8(e, PSX_R2, 1);
            psx_jit_st_reg(e, PSX_R2, rt);

            return 1;
        }

        psx_emit_imm32(e, PSX_R1, imm);

        switch (o)
        {
        case 0x0c: psx_emit_and_reg(e, PSX_R0, PSX_R0, PSX_R1); break;
        case 0x0d: psx_emit_orr_reg(e, PSX_R0, PSX_R0, PSX_R1); break;
        case 0x0e: psx_emit_eor_reg(e, PSX_R0, PSX_R0, PSX_R1); break;
        default:
            psx_emit_mov_imm8(e, PSX_R2, 0);
            psx_emit_cmp_reg(e, PSX_R0, PSX_R1);
            psx_emit_it(e, (o == 0x0a) ? PSX_CC_LT : PSX_CC_CC);
            psx_emit_mov_imm8(e, PSX_R2, 1);
            psx_emit_mov(e, PSX_R0, PSX_R2);
            break;
        }

        psx_jit_st_reg(e, PSX_R0, rt);

        return 1;
    }

    case 0x0f: /* LUI */
    {
        if (rt == 0)
            return 1;

        psx_emit_const(e, PSX_R0, PSX_IMM(op) << 16);
        psx_jit_st_reg(e, PSX_R0, rt);

        return 1;
    }

    default:
        return 0;
    }
}

/*
    The ALU with what the translator knows about the registers: an instruction
    whose inputs are all known becomes the store of its result, and a LUI that
    the next instruction completes (LUI + ORI / ADDIU into the same register,
    which is how every 32 bit constant is made) is emitted together with it as
    one constant. Returns 2 when the next instruction was consumed as well.
*/
static inline int psx_jit_translate_alu_c(psx_jit_ctx_t *c, uint32_t op)
{
    uint32_t dst = 0;
    uint32_t value = 0;

    if (psx_jit_fold_alu(c, op, &dst, &value))
    {
        if (!dst)
            return 1;

        /* LUI rX ; ORI / ADDIU rX, rX, imm - nothing can look at rX in between
           (not even the TTY hook, which runs before the instruction at 0xb4) */
        if ((PSX_OP(op) == 0x0f) && (c->next_op != 0xffffffffu) && !c->in_delay &&
            !PSX_JIT_IS_BHOOK_PC(c->guest + 4u))
        {
            const uint32_t n = c->next_op;
            const uint32_t no = PSX_OP(n);

            if (((no == 0x0d) || (no == 0x09)) && (PSX_RS(n) == dst) && (PSX_RT(n) == dst))
            {
                const uint32_t v2 = (no == 0x0d) ? (value | PSX_IMM(n)) : (value + PSX_SIMM(n));

                psx_jit_st_const(c, dst, v2);
                return 2;
            }
        }

        psx_jit_st_const(c, dst, value);
        return 1;
    }

    if (!psx_jit_translate_alu(c->e, op))
        return 0;

    psx_jit_kill_written(c, op);

    return 1;
}

/* ------------------------------------------------------- trapping arithmetic */

/*
    ADD, ADDI and SUB raise an overflow exception, so the native form keeps the
    flags and branches to a stub when V is set - the stub runs the instruction in
    the interpreter, which raises the exception with the correct EPC.
*/
static inline int psx_jit_is_trap_alu(uint32_t op)
{
    const uint32_t o = PSX_OP(op);

    return (o == 0x08) || ((o == 0x00) && ((PSX_FN(op) == 0x20) || (PSX_FN(op) == 0x22)));
}

/* in_delay: the instruction is the delay slot of the branch being translated -
   r3 is taken, and an overflow has to reach the interpreter as a delay slot */
static inline int psx_jit_translate_trap_alu_ex(psx_jit_ctx_t *c, uint32_t op, int in_delay)
{
    psx_emit_t *const e = c->e;

    const uint32_t o = PSX_OP(op);
    const uint32_t rs = PSX_RS(op);
    const uint32_t rt = PSX_RT(op);
    const uint32_t rd = PSX_RD(op);
    const uint32_t fn = PSX_FN(op);

    uint32_t dst;
    int sub = 0;

    if (o == 0x08) /* ADDI */
    {
        dst = rt;
    }
    else if ((o == 0x00) && ((fn == 0x20) || (fn == 0x22))) /* ADD / SUB */
    {
        dst = rd;
        sub = (fn == 0x22);
    }
    else
    {
        return 0;
    }

    psx_jit_ld_reg(e, PSX_R0, rs);

    if (o == 0x08)
    {
        const uint32_t imm = PSX_SIMM(op);
        const uint32_t imm12 = psx_thumb_expand_imm(imm);

        if (imm12 != 0xffffffffu)
        {
            psx_emit_adds_imm12(e, PSX_R0, PSX_R0, imm12);
        }
        else
        {
            psx_emit_imm32(e, PSX_R1, imm);
            psx_emit_adds_reg(e, PSX_R0, PSX_R0, PSX_R1);
        }
    }
    else
    {
        psx_jit_ld_reg(e, PSX_R1, rt);

        if (sub)
            psx_emit_subs_reg(e, PSX_R0, PSX_R0, PSX_R1);
        else
            psx_emit_adds_reg(e, PSX_R0, PSX_R0, PSX_R1);
    }

    psx_jit_stub_t *const s = psx_jit_new_stub(c, in_delay ? PSX_JIT_STUB_DELAY : PSX_JIT_STUB_TRAP);

    s->site[0] = psx_jit_stub_site(c, PSX_CC_VS);
    s->cond[0] = PSX_CC_VS;
    s->data = (c->exit_mode == PSX_JIT_EXIT_LINK) ? PSX_JIT_DF_LINK : 0u;

    psx_jit_st_reg(e, PSX_R0, dst);
    psx_jit_kill_const(c, dst);

    return 1;
}

static inline int psx_jit_translate_trap_alu(psx_jit_ctx_t *c, uint32_t op)
{
    return psx_jit_translate_trap_alu_ex(c, op, 0);
}

/* --------------------------------------------------------------- GTE */

/*
    GTE commands become one call. The helper returns the cycle count, which is
    why the block takes r0 off the cycle counter instead of carrying a copy of
    the timing table.

    The register moves are not: 3D code does several of them around every
    command (some 600 000 a second in an FF7 battle), and all but a handful of
    the registers are a plain field of psx_cpu_t, so a move is one load or store
    here. The table below says how each register is kept - it has to match
    gte_read_register() and gte_write_register() in cpu.c, which stay the
    reference and still serve the registers with side effects (SXYP, IRGB,
    LZCS, FLAG).

    MTC2 / CTC2 are plain writes. MFC2 / CFC2 are loads, with the load delay of
    one: like a memory load, the value goes straight into the guest register
    when the next instruction does not touch it, and into the load delay slot -
    with the next instruction interpreted, which applies it - when it does.
*/
enum
{
    PSX_GTE_RD_32,   /* the whole word                        */
    PSX_GTE_RD_S16,  /* 16 bit field, read back sign extended */
    PSX_GTE_RD_U16,  /* 16 bit field, read back zero extended */
    PSX_GTE_RD_CALL, /* psx_cpu_gte_read()                    */
};

enum
{
    PSX_GTE_WR_32,
    PSX_GTE_WR_16,
    PSX_GTE_WR_NONE, /* read only: the write is dropped */
    PSX_GTE_WR_CALL, /* psx_cpu_gte_write()             */
};

typedef struct
{
    uint16_t off; /* of the field in psx_cpu_t */
    uint8_t rd, wr;
} psx_jit_gte_reg_t;

#define PSX_GTE_D(field, rd, wr) {(uint16_t)offsetof(psx_cpu_t, cop2_dr.field), (rd), (wr)}
#define PSX_GTE_C(field, rd, wr) {(uint16_t)offsetof(psx_cpu_t, cop2_cr.field), (rd), (wr)}

static const psx_jit_gte_reg_t g_psx_jit_gte_regs[64] = {
    /* data registers */
    PSX_GTE_D(v[0].xy, PSX_GTE_RD_32, PSX_GTE_WR_32),     /*  0 VXY0 */
    PSX_GTE_D(v[0].z, PSX_GTE_RD_S16, PSX_GTE_WR_16),     /*  1 VZ0  */
    PSX_GTE_D(v[1].xy, PSX_GTE_RD_32, PSX_GTE_WR_32),     /*  2 VXY1 */
    PSX_GTE_D(v[1].z, PSX_GTE_RD_S16, PSX_GTE_WR_16),     /*  3 VZ1  */
    PSX_GTE_D(v[2].xy, PSX_GTE_RD_32, PSX_GTE_WR_32),     /*  4 VXY2 */
    PSX_GTE_D(v[2].z, PSX_GTE_RD_S16, PSX_GTE_WR_16),     /*  5 VZ2  */
    PSX_GTE_D(rgbc.rgbc, PSX_GTE_RD_32, PSX_GTE_WR_32),   /*  6 RGBC */
    PSX_GTE_D(otz, PSX_GTE_RD_U16, PSX_GTE_WR_16),        /*  7 OTZ  */
    PSX_GTE_D(ir[0], PSX_GTE_RD_S16, PSX_GTE_WR_16),      /*  8 IR0  */
    PSX_GTE_D(ir[1], PSX_GTE_RD_S16, PSX_GTE_WR_16),      /*  9 IR1  */
    PSX_GTE_D(ir[2], PSX_GTE_RD_S16, PSX_GTE_WR_16),      /* 10 IR2  */
    PSX_GTE_D(ir[3], PSX_GTE_RD_S16, PSX_GTE_WR_16),      /* 11 IR3  */
    PSX_GTE_D(sxy[0].xy, PSX_GTE_RD_32, PSX_GTE_WR_32),   /* 12 SXY0 */
    PSX_GTE_D(sxy[1].xy, PSX_GTE_RD_32, PSX_GTE_WR_32),   /* 13 SXY1 */
    PSX_GTE_D(sxy[2].xy, PSX_GTE_RD_32, PSX_GTE_WR_32),   /* 14 SXY2 */
    PSX_GTE_D(sxy[2].xy, PSX_GTE_RD_32, PSX_GTE_WR_CALL), /* 15 SXYP: reads SXY2, a write pushes */
    PSX_GTE_D(sz[0], PSX_GTE_RD_U16, PSX_GTE_WR_16),      /* 16 SZ0  */
    PSX_GTE_D(sz[1], PSX_GTE_RD_U16, PSX_GTE_WR_16),      /* 17 SZ1  */
    PSX_GTE_D(sz[2], PSX_GTE_RD_U16, PSX_GTE_WR_16),      /* 18 SZ2  */
    PSX_GTE_D(sz[3], PSX_GTE_RD_U16, PSX_GTE_WR_16),      /* 19 SZ3  */
    PSX_GTE_D(rgb[0].rgbc, PSX_GTE_RD_32, PSX_GTE_WR_32), /* 20 RGB0 */
    PSX_GTE_D(rgb[1].rgbc, PSX_GTE_RD_32, PSX_GTE_WR_32), /* 21 RGB1 */
    PSX_GTE_D(rgb[2].rgbc, PSX_GTE_RD_32, PSX_GTE_WR_32), /* 22 RGB2 */
    PSX_GTE_D(res1, PSX_GTE_RD_32, PSX_GTE_WR_32),        /* 23      */
    PSX_GTE_D(mac[0], PSX_GTE_RD_32, PSX_GTE_WR_32),      /* 24 MAC0 */
    PSX_GTE_D(mac[1], PSX_GTE_RD_32, PSX_GTE_WR_32),      /* 25 MAC1 */
    PSX_GTE_D(mac[2], PSX_GTE_RD_32, PSX_GTE_WR_32),      /* 26 MAC2 */
    PSX_GTE_D(mac[3], PSX_GTE_RD_32, PSX_GTE_WR_32),      /* 27 MAC3 */
    PSX_GTE_D(irgb, PSX_GTE_RD_CALL, PSX_GTE_WR_CALL),    /* 28 IRGB: both ways go through IR1-3 */
    PSX_GTE_D(irgb, PSX_GTE_RD_U16, PSX_GTE_WR_NONE),     /* 29 ORGB: IRGB as it stands */
    PSX_GTE_D(lzcs, PSX_GTE_RD_32, PSX_GTE_WR_CALL),      /* 30 LZCS: a write counts into LZCR */
    PSX_GTE_D(lzcr, PSX_GTE_RD_32, PSX_GTE_WR_NONE),      /* 31 LZCR */

    /* control registers */
    PSX_GTE_C(rt.m[0].u32, PSX_GTE_RD_32, PSX_GTE_WR_32), /* 32 */
    PSX_GTE_C(rt.m[1].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(rt.m[2].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(rt.m[3].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(rt.m33, PSX_GTE_RD_S16, PSX_GTE_WR_16),
    PSX_GTE_C(tr.x, PSX_GTE_RD_32, PSX_GTE_WR_32),        /* 37 */
    PSX_GTE_C(tr.y, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(tr.z, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(l.m[0].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),  /* 40 */
    PSX_GTE_C(l.m[1].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(l.m[2].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(l.m[3].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(l.m33, PSX_GTE_RD_S16, PSX_GTE_WR_16),
    PSX_GTE_C(bk.x, PSX_GTE_RD_32, PSX_GTE_WR_32),        /* 45 */
    PSX_GTE_C(bk.y, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(bk.z, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(lr.m[0].u32, PSX_GTE_RD_32, PSX_GTE_WR_32), /* 48 */
    PSX_GTE_C(lr.m[1].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(lr.m[2].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(lr.m[3].u32, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(lr.m33, PSX_GTE_RD_S16, PSX_GTE_WR_16),
    PSX_GTE_C(fc.x, PSX_GTE_RD_32, PSX_GTE_WR_32),        /* 53 */
    PSX_GTE_C(fc.y, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(fc.z, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(ofx, PSX_GTE_RD_32, PSX_GTE_WR_32),         /* 56 */
    PSX_GTE_C(ofy, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(h, PSX_GTE_RD_S16, PSX_GTE_WR_32),          /* 58 H: kept whole, read back as int16 */
    PSX_GTE_C(dqa, PSX_GTE_RD_S16, PSX_GTE_WR_16),
    PSX_GTE_C(dqb, PSX_GTE_RD_32, PSX_GTE_WR_32),
    PSX_GTE_C(zsf3, PSX_GTE_RD_S16, PSX_GTE_WR_16),
    PSX_GTE_C(zsf4, PSX_GTE_RD_S16, PSX_GTE_WR_16),
    PSX_GTE_C(flag, PSX_GTE_RD_CALL, PSX_GTE_WR_CALL),    /* 63 FLAG: masked, bit 31 made up on read */
};

#undef PSX_GTE_D
#undef PSX_GTE_C

/* Around a helper call inside a branch delay slot r3 has to survive */
static inline void psx_jit_call_keep_r3(psx_jit_ctx_t *c, uint32_t slot)
{
    if (c->in_delay)
        psx_emit_mov(c->e, PSX_JIT_KEEP, PSX_R3);

    psx_jit_emit_call(c, slot);

    if (c->in_delay)
        psx_emit_mov(c->e, PSX_R3, PSX_JIT_KEEP);
}

static inline int psx_jit_translate_cop2(psx_jit_ctx_t *c, uint32_t op)
{
    if (PSX_OP(op) != 0x12)
        return 0;

    psx_emit_t *const e = c->e;

    if (!(op & 0x02000000u))
    {
        const uint32_t rt = PSX_RT(op);
        const uint32_t rd = PSX_RD(op);

        switch (PSX_RS(op))
        {
        case 0x04: /* MTC2 */
        case 0x06: /* CTC2 */
        {
            const uint32_t reg = rd + ((PSX_RS(op) == 0x06) ? 32u : 0u);
            const psx_jit_gte_reg_t *const g = &g_psx_jit_gte_regs[reg];

            switch (g->wr)
            {
            case PSX_GTE_WR_32:
                psx_jit_ld_reg(e, PSX_R0, rt);
                psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU, g->off);
                break;

            case PSX_GTE_WR_16:
                psx_jit_ld_reg(e, PSX_R0, rt);
                psx_emit_strh_imm(e, PSX_R0, PSX_JIT_CPU, g->off);
                break;

            case PSX_GTE_WR_NONE:
                break;

            default:
                psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
                psx_emit_mov_imm8(e, PSX_R1, reg);
                psx_jit_ld_reg(e, PSX_R2, rt);
                psx_jit_call_keep_r3(c, PSX_JIT_H_GTE_WRITE);
                break;
            }

            return 1;
        }

        case 0x00: /* MFC2 */
        case 0x02: /* CFC2 */
        {
            const uint32_t reg = rd + ((PSX_RS(op) == 0x02) ? 32u : 0u);
            const psx_jit_gte_reg_t *const g = &g_psx_jit_gte_regs[reg];

            if (g->rd == PSX_GTE_RD_CALL)
            {
                /* called even when the value goes nowhere: reading IRGB
                   rebuilds it from IR1-3, which ORGB shows afterwards */
                psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
                psx_emit_mov_imm8(e, PSX_R1, reg);
                psx_jit_call_keep_r3(c, PSX_JIT_H_GTE_READ);
            }

            if (rt == 0)
                return 1; /* nowhere to go */

            psx_jit_kill_const(c, rt);

            if (g->rd == PSX_GTE_RD_32)
                psx_emit_ldr_imm(e, PSX_R0, PSX_JIT_CPU, g->off);
            else if (g->rd == PSX_GTE_RD_S16)
                psx_emit_ldrsh_imm(e, PSX_R0, PSX_JIT_CPU, g->off);
            else if (g->rd == PSX_GTE_RD_U16)
                psx_emit_ldrh_imm(e, PSX_R0, PSX_JIT_CPU, g->off);

            if (psx_jit_reads_reg(c->next_op, rt) || psx_jit_writes_reg(c->next_op, rt))
            {
                psx_emit_mov_imm8(e, PSX_R1, rt);
                psx_emit_strd_imm(e, PSX_R1, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_D);

                c->force_next = 1;
            }
            else
            {
                psx_jit_st_reg(e, PSX_R0, rt);
            }

            return 1;
        }

        default:
            return 0;
        }
    }

    psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
    psx_emit_imm32(e, PSX_R1, op);
    psx_jit_call_keep_r3(c, PSX_JIT_H_GTE);
    psx_emit_sub_reg(e, PSX_JIT_CYC, PSX_JIT_CYC, PSX_R0);

    c->cycles_done = 1;

    return 1;
}

/* --------------------------------------------------------------- COP0 */

/* What MTC0 lets through, per register - the same as g_psx_cpu_cop0_write_mask_table
   in cpu.c, which the interpreter uses. */
static const uint32_t g_psx_jit_cop0_mask[16] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0x00000000u, 0xffffffffu, 0x00000000u, 0xffc0f03fu,
    0x00000000u, 0xffffffffu, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000300u, 0x00000000u, 0x00000000u,
};

static inline int psx_jit_is_rfe(uint32_t op)
{
    return (PSX_OP(op) == 0x10u) && (PSX_RS(op) == 0x10u) && (PSX_FN(op) == 0x10u);
}

/* RFE: the interrupt enable / mode stack pops one level (r0, r1 used) */
static inline void psx_jit_emit_rfe(psx_emit_t *e)
{
    psx_emit_ldr_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_COP0(COP0_SR));
    psx_emit_and_imm12(e, PSX_R1, PSX_R0, 0x3cu);                   /* (sr & 0x3f) >> 2 is (sr & 0x3c) >> 2 */
    psx_emit_bic_imm12(e, PSX_R0, PSX_R0, 0x0fu);
    psx_emit_shift_imm(e, 1, PSX_R1, PSX_R1, 2);
    psx_emit_orr_reg(e, PSX_R0, PSX_R0, PSX_R1);
    psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_COP0(COP0_SR));
}

/*
    MFC0, MTC0 and RFE. The interrupt handler of every game runs through them,
    and each one interpreted used to take the instruction behind it along (the
    interpreter may have left a load pending).

    MFC0 is a load with the load delay, like MFC2. MTC0 into SR or CAUSE and RFE
    can make an interrupt pending and enabled, or isolate the cache - both are
    for the dispatcher to deal with, so the block tests for them and leaves
    behind the instruction when either happened.
*/
static inline int psx_jit_translate_cop0(psx_jit_ctx_t *c, uint32_t op)
{
    if (PSX_OP(op) != 0x10u)
        return 0;

    psx_emit_t *const e = c->e;

    const uint32_t rs = PSX_RS(op);
    const uint32_t rt = PSX_RT(op);
    const uint32_t rd = PSX_RD(op);

    int check = 0;

    if (rs == 0x00u) /* MFC0 */
    {
        if (rd >= 16u)
            return 0; /* no such register: whatever the interpreter makes of it */

        if (rt == 0)
            return 1;

        psx_jit_kill_const(c, rt);

        psx_emit_ldr_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_COP0(rd));

        if (psx_jit_reads_reg(c->next_op, rt) || psx_jit_writes_reg(c->next_op, rt))
        {
            psx_emit_mov_imm8(e, PSX_R1, rt);
            psx_emit_strd_imm(e, PSX_R1, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_D);

            c->force_next = 1;
        }
        else
        {
            psx_jit_st_reg(e, PSX_R0, rt);
        }

        return 1;
    }

    if (rs == 0x04u) /* MTC0 */
    {
        if (rd >= 16u)
            return 0;

        const uint32_t mask = g_psx_jit_cop0_mask[rd];

        if (mask == 0u)
        {
            psx_emit_mov_imm8(e, PSX_R0, 0);
        }
        else
        {
            psx_jit_ld_reg(e, PSX_R0, rt);

            if (mask != 0xffffffffu)
            {
                const uint32_t imm12 = psx_thumb_expand_imm(mask);

                if (imm12 != 0xffffffffu)
                {
                    psx_emit_and_imm12(e, PSX_R0, PSX_R0, imm12);
                }
                else
                {
                    psx_emit_imm32(e, PSX_R1, mask);
                    psx_emit_and_reg(e, PSX_R0, PSX_R0, PSX_R1);
                }
            }
        }

        psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_COP0(rd));

        check = (rd == COP0_SR) || (rd == COP0_CAUSE);
    }
    else if (psx_jit_is_rfe(op))
    {
        psx_jit_emit_rfe(e);
        check = 1;
    }
    else
    {
        return 0;
    }

    if (check)
    {
        /* an interrupt pending and enabled, or the cache isolated: leave */
        psx_jit_stub_t *const s = psx_jit_new_stub(c, PSX_JIT_STUB_LEAVE);

        psx_emit_ldr_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_COP0(COP0_SR));
        psx_emit_ldr_imm(e, PSX_R1, PSX_JIT_CPU, PSX_JIT_OFF_COP0(COP0_CAUSE));
        psx_emit_and_reg(e, PSX_R1, PSX_R1, PSX_R0);
        psx_emit_tst_imm12(e, PSX_R1, psx_thumb_expand_imm(0x700u));
        psx_emit_it(e, PSX_CC_NE);
        psx_emit_tst_imm12(e, PSX_R0, psx_thumb_expand_imm(SR_IEC));

        s->site[0] = psx_jit_stub_site(c, PSX_CC_NE);

        psx_emit_tst_imm12(e, PSX_R0, psx_thumb_expand_imm(SR_ISC));

        s->site[1] = psx_jit_stub_site(c, PSX_CC_NE);
    }

    return 1;
}

/* ------------------------------------------------------------------ memory */

/*
    The address of a load or store into r0 and, when it is plain guest RAM, its
    offset in the RAM buffer into r1. What is not - the scratchpad, I/O, the BIOS,
    a misaligned address that has to raise an address error - branches to the
    stub, whose sites are filled in.

      LSLS r1, r0, #3      the segment bits are gone: RAM <=> bits 31:24 zero
      LSRS r2, r1, #24
      BNE.W stub
      LSLS r2, r0, #30/31  halfwords and words: misaligned goes the long way
      BNE.W stub
      LSRS r1, r1, #3
*/
static inline void psx_jit_emit_addr(psx_jit_ctx_t *c, uint32_t rs, uint32_t simm, uint32_t size,
                                     psx_jit_stub_t *s)
{
    psx_emit_t *const e = c->e;

    psx_jit_ld_reg(e, PSX_R0, rs);

    if (simm)
    {
        if (simm < 0x1000u)
            psx_emit_add_imm12(e, PSX_R0, PSX_R0, simm);
        else if ((uint32_t)(0u - simm) < 0x1000u)
            psx_emit_sub_imm12(e, PSX_R0, PSX_R0, 0u - simm);
        else
        {
            psx_emit_imm32(e, PSX_R1, simm);
            psx_emit_add_reg(e, PSX_R0, PSX_R0, PSX_R1);
        }
    }

    psx_emit_shift_imm(e, 0, PSX_R1, PSX_R0, 3);
    psx_emit_shift_imm(e, 1, PSX_R2, PSX_R1, 24);

    s->site[0] = psx_jit_stub_site(c, PSX_CC_NE);

    if (size > 1u)
    {
        psx_emit_shift_imm(e, 0, PSX_R2, PSX_R0, (size == 4u) ? 30u : 31u);

        s->site[1] = psx_jit_stub_site(c, PSX_CC_NE);
    }

    psx_emit_shift_imm(e, 1, PSX_R1, PSX_R1, 3);
}

/* Can this access be translated at all? Used to decide whether a branch delay
   slot can be taken natively, before any code has been emitted - so it has to
   agree with every early return of psx_jit_translate_mem_ex: once the branch
   has been emitted the delay slot can no longer be handed to the interpreter. */
static inline int psx_jit_mem_supported(uint32_t op)
{
    switch (PSX_OP(op))
    {
    case 0x20:
    case 0x21:
    case 0x23:
    case 0x24:
    case 0x25:
        return PSX_RT(op) != 0; /* a load into r0 is left to the interpreter */

    case 0x28:
    case 0x29:
    case 0x2b:
        return 1;

    default:
        return 0;
    }
}

/* translate_mem modes */
#define PSX_JIT_MEM_DELAY 1u   /* the access sits in a branch delay slot      */
#define PSX_JIT_MEM_PENDING 2u /* a load leaves its value in the delay slot   */

/* The loaded value (r2) goes to its register - or into the load delay slot,
   for the next instruction (which is then interpreted) to apply. */
static inline void psx_jit_emit_load_result(psx_jit_ctx_t *c, uint32_t rt, uint32_t flags)
{
    if (flags & PSX_JIT_MEM_PENDING)
    {
        psx_emit_mov_imm8(c->e, PSX_R1, rt);
        psx_emit_strd_imm(c->e, PSX_R1, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_D);
    }
    else
    {
        psx_jit_st_reg(c->e, PSX_R2, rt);
    }
}

/* the description word of a PSX_JIT_H_MEM stub, without the back offset */
static inline uint32_t psx_jit_mem_desc(const psx_jit_ctx_t *c, uint32_t rt, uint32_t size, int is_signed,
                                        int is_load, uint32_t flags)
{
    uint32_t d = rt | ((size == 4u) ? 0x40u : ((size == 2u) ? 0x20u : 0u)) |
                 (is_signed ? PSX_JIT_MD_SIGNED : 0u) | (is_load ? 0u : PSX_JIT_MD_STORE) |
                 (((c->guest - c->block_pc) >> 2) << 9);

    if (flags & PSX_JIT_MEM_DELAY)
    {
        d |= PSX_JIT_MD_DELAY;

        if (c->exit_mode == PSX_JIT_EXIT_LINK)
            d |= PSX_JIT_MD_LINK;
    }

    return d;
}

/* LB / LBU / LH / LHU / LW / SB / SH / SW */
static inline int psx_jit_translate_mem_ex(psx_jit_ctx_t *c, uint32_t op, uint32_t flags)
{
    psx_emit_t *const e = c->e;

    const uint32_t o = PSX_OP(op);
    const uint32_t rs = PSX_RS(op);
    const uint32_t rt = PSX_RT(op);
    const uint32_t simm = PSX_SIMM(op);

    int is_load = 0;
    uint32_t size = 0;
    int is_signed = 0;

    switch (o)
    {
    case 0x20: is_load = 1; size = 1; is_signed = 1; break; /* LB  */
    case 0x24: is_load = 1; size = 1; is_signed = 0; break; /* LBU */
    case 0x21: is_load = 1; size = 2; is_signed = 1; break; /* LH  */
    case 0x25: is_load = 1; size = 2; is_signed = 0; break; /* LHU */
    case 0x23: is_load = 1; size = 4; is_signed = 0; break; /* LW  */
    case 0x28: is_load = 0; size = 1; break;                /* SB  */
    case 0x29: is_load = 0; size = 2; break;                /* SH  */
    case 0x2b: is_load = 0; size = 4; break;                /* SW  */
    default:
        return 0;
    }

    if (is_load && (rt == 0))
        return 0; /* a load into r0 is a bus read with no effect: not worth it */

    if (c->cycles > 63u)
        psx_jit_flush_cycles(c); /* the stub's description has six bits for them */

    const uint32_t desc = psx_jit_mem_desc(c, rt, size, is_signed, is_load, flags);

    /*
        The address is known: the base register holds a constant (LUI + access,
        how globals are reached). RAM and the scratchpad are then accessed with
        no test at all; anything else goes straight to the helper.
    */
    if (psx_jit_is_const(c, rs))
    {
        const uint32_t addr = psx_jit_const_of(c, rs) + simm;

        if (addr & (size - 1u))
            return 0; /* raises an address error: the interpreter's business */

        const uint32_t phys = addr & 0x1fffffffu;
        const uint32_t seg = addr >> 29;
        const int kseg = (seg == 0u) || (seg == 4u) || (seg == 5u);

        if (!(addr & PSX_JIT_RAM_WINDOW_MASK))
        {
            /* plain RAM, the same window as the test at run time */
            const uint32_t off = addr & 0x1fffffu;

            if (is_load)
            {
                if (off < 0x1000u)
                {
                    psx_emit_load_imm(e, size, is_signed, PSX_R2, PSX_JIT_RAM, off);
                }
                else
                {
                    psx_emit_const(e, PSX_R1, off);
                    psx_emit_load_reg(e, size, is_signed, PSX_R2, PSX_JIT_RAM, PSX_R1);
                }

                psx_jit_emit_load_result(c, rt, flags);
                psx_jit_kill_const(c, rt);

                return 1;
            }

            /* a store still asks whether code was translated from there */
            psx_jit_stub_t *const s = psx_jit_new_stub(c, PSX_JIT_STUB_MEM);

            s->data = desc;
            s->set_addr = 1;
            s->addr = addr;

            const uint32_t flag = off >> PSX_JIT_CODE_FLAG_SHIFT;

            if (flag < 0x1000u)
            {
                psx_emit_ldrb_imm(e, PSX_R2, PSX_JIT_PAGES, flag);
            }
            else
            {
                psx_emit_const(e, PSX_R2, flag);
                psx_emit_ldrb_reg(e, PSX_R2, PSX_JIT_PAGES, PSX_R2);
            }

            psx_emit_cmp_imm8(e, PSX_R2, 0);

            s->site[0] = psx_jit_stub_site(c, PSX_CC_NE);

            psx_jit_ld_reg(e, PSX_R2, rt);

            if (off < 0x1000u)
            {
                psx_emit_store_imm(e, size, PSX_R2, PSX_JIT_RAM, off);
            }
            else
            {
                psx_emit_const(e, PSX_R1, off);
                psx_emit_store_reg(e, size, PSX_R2, PSX_JIT_RAM, PSX_R1);
            }

            s->resume = e->cur;

            return 1;
        }

        if (kseg && ((phys - 0x1f800000u) < 0x400u))
        {
            /* the scratchpad: its buffer is in the helper table */
            const uint32_t off = phys - 0x1f800000u;

            psx_emit_ldr_imm(e, PSX_R1, PSX_JIT_HELPERS, PSX_JIT_H_SPAD);

            if (is_load)
            {
                psx_emit_load_imm(e, size, is_signed, PSX_R2, PSX_R1, off);
                psx_jit_emit_load_result(c, rt, flags);
                psx_jit_kill_const(c, rt);
            }
            else
            {
                psx_jit_ld_reg(e, PSX_R2, rt);
                psx_emit_store_imm(e, size, PSX_R2, PSX_R1, off);
            }

            return 1;
        }

        /* a device register: no test worth making, the helper does it all */
        psx_jit_stub_t *const s = psx_jit_new_stub(c, PSX_JIT_STUB_MEM);

        s->data = desc;
        s->set_addr = 1;
        s->addr = addr;
        s->site[0] = psx_jit_stub_site(c, PSX_CC_AL);
        s->cond[0] = PSX_CC_AL;
        s->resume = e->cur;

        if (is_load)
        {
            psx_jit_emit_load_result(c, rt, flags);
            psx_jit_kill_const(c, rt);
        }

        return 1;
    }

    psx_jit_stub_t *const s = psx_jit_new_stub(c, PSX_JIT_STUB_MEM);

    s->data = desc;

    psx_jit_emit_addr(c, rs, simm, size, s);

    if (is_load)
    {
        psx_emit_load_reg(e, size, is_signed, PSX_R2, PSX_JIT_RAM, PSX_R1);

        /* the stub comes back here with the value in r2 */
        s->resume = e->cur;

        psx_jit_emit_load_result(c, rt, flags);
        psx_jit_kill_const(c, rt);
    }
    else
    {
        /* A store into a page some block was translated from has to invalidate
           it, and the helper is what does that. r2 is free until the value is
           needed, r3 may carry where the branch this is the delay slot of goes. */
        psx_emit_shift_imm(e, 1, PSX_R2, PSX_R1, PSX_JIT_CODE_FLAG_SHIFT);
        psx_emit_ldrb_reg(e, PSX_R2, PSX_JIT_PAGES, PSX_R2);
        psx_emit_cmp_imm8(e, PSX_R2, 0);

        s->site[2] = psx_jit_stub_site(c, PSX_CC_NE);

        psx_jit_ld_reg(e, PSX_R2, rt);
        psx_emit_store_reg(e, size, PSX_R2, PSX_JIT_RAM, PSX_R1);

        s->resume = e->cur;
    }

    return 1;
}

/* Plain, non delay slot access. A load whose result would be visible one
   instruction too early is left pending instead, and the instruction that
   follows is interpreted - it applies it. */
static inline int psx_jit_translate_mem(psx_jit_ctx_t *c, uint32_t op)
{
    uint32_t flags = 0;

    const uint32_t o = PSX_OP(op);

    if ((o == 0x20) || (o == 0x21) || (o == 0x23) || (o == 0x24) || (o == 0x25))
    {
        const uint32_t rt = PSX_RT(op);

        if (psx_jit_reads_reg(c->next_op, rt) || psx_jit_writes_reg(c->next_op, rt))
        {
            flags |= PSX_JIT_MEM_PENDING;
            c->force_next = 1;
        }
    }

    const int ok = psx_jit_translate_mem_ex(c, op, flags);

    if (!ok)
        c->force_next = 0;

    return ok;
}

/*
    LWC2 / SWC2. Vertices go into the GTE and screen coordinates come out of it
    this way, three of each for a triangle, so these are as frequent as the
    register moves. Against guest RAM, and for a register that is a plain field
    (see the table above), the transfer is done here: the address test of an
    ordinary load or store, then a load and a store. Everything else - another
    address, a misaligned one, a store into a page code was translated from, a
    register with side effects - takes the stub into the interpreter handlers,
    see psx_cpu_gte_transfer.

    The stub serves the scratchpad itself, where Tekken 3 puts its screen
    coordinates (one SWC2 per vertex, the last one of a triangle in the delay
    slot of the loop branch). In a delay slot anything else is the interpreter
    running it with the branch pending, which leaves the block.
*/
static inline int psx_jit_translate_gte_mem(psx_jit_ctx_t *c, uint32_t op)
{
    if ((PSX_OP(op) != 0x32u) && (PSX_OP(op) != 0x3au))
        return 0;

    psx_emit_t *const e = c->e;

    const psx_jit_gte_reg_t *const g = &g_psx_jit_gte_regs[PSX_RT(op)]; /* data registers only */
    const int is_load = (PSX_OP(op) == 0x32u);

    psx_jit_stub_t *const s = psx_jit_new_stub(c, PSX_JIT_STUB_GTE_MEM);

    s->data = op;

    if (c->in_delay)
        s->pc |= PSX_JIT_GF_DELAY | ((c->exit_mode == PSX_JIT_EXIT_LINK) ? PSX_JIT_GF_LINK : 0u);

    if (is_load ? (g->wr != PSX_GTE_WR_CALL) : (g->rd != PSX_GTE_RD_CALL))
    {
        psx_jit_emit_addr(c, PSX_RS(op), PSX_SIMM(op), 4u, s);

        if (is_load)
        {
            if (g->wr != PSX_GTE_WR_NONE)
            {
                psx_emit_ldr_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);

                if (g->wr == PSX_GTE_WR_32)
                    psx_emit_str_imm(e, PSX_R2, PSX_JIT_CPU, g->off);
                else
                    psx_emit_strh_imm(e, PSX_R2, PSX_JIT_CPU, g->off);
            }
        }
        else
        {
            /* a page with translated code in it goes the long way, which is
               what invalidates */
            psx_emit_shift_imm(e, 1, PSX_R2, PSX_R1, PSX_JIT_CODE_FLAG_SHIFT);
            psx_emit_ldrb_reg(e, PSX_R2, PSX_JIT_PAGES, PSX_R2);
            psx_emit_cmp_imm8(e, PSX_R2, 0);

            s->site[2] = psx_jit_stub_site(c, PSX_CC_NE);

            if (g->rd == PSX_GTE_RD_32)
                psx_emit_ldr_imm(e, PSX_R2, PSX_JIT_CPU, g->off);
            else if (g->rd == PSX_GTE_RD_S16)
                psx_emit_ldrsh_imm(e, PSX_R2, PSX_JIT_CPU, g->off);
            else
                psx_emit_ldrh_imm(e, PSX_R2, PSX_JIT_CPU, g->off);

            psx_emit_str_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
        }
    }
    else
    {
        /* a register with side effects: always the stub, which wants the
           address in r0 */
        psx_jit_ld_reg(e, PSX_R0, PSX_RS(op));

        if (PSX_SIMM(op))
        {
            psx_emit_imm32(e, PSX_R1, PSX_SIMM(op));
            psx_emit_add_reg(e, PSX_R0, PSX_R0, PSX_R1);
        }

        s->site[0] = psx_jit_stub_site(c, PSX_CC_AL);
        s->cond[0] = PSX_CC_AL;
    }

    s->resume = e->cur;

    return 1;
}

/* ------------------------------------------------------------- divide */

/*
    DIV and DIVU, with the PSX results for a zero divisor. The overflow case
    (0x80000000 / -1) needs no check: SDIV returns 0x80000000 and the MLS
    remainder is zero, which is exactly what the interpreter produces.

    This one branches, so it is not offered for delay slots.
*/
static inline int psx_jit_translate_div(psx_jit_ctx_t *c, uint32_t op)
{
    psx_emit_t *const e = c->e;

    if ((PSX_OP(op) != 0x00) || ((PSX_FN(op) != 0x1a) && (PSX_FN(op) != 0x1b)))
        return 0;

    const int is_signed = (PSX_FN(op) == 0x1a);

    psx_jit_ld_reg(e, PSX_R0, PSX_RS(op));
    psx_jit_ld_reg(e, PSX_R1, PSX_RT(op));

    psx_emit_cmp_imm8(e, PSX_R1, 0);

    uint16_t *zero = psx_emit_bcond_short_fwd(e, PSX_CC_EQ);

    if (is_signed)
        psx_emit_sdiv(e, PSX_R2, PSX_R0, PSX_R1);
    else
        psx_emit_udiv(e, PSX_R2, PSX_R0, PSX_R1);

    psx_emit_mls(e, PSX_R3, PSX_R2, PSX_R1, PSX_R0); /* remainder */

    psx_emit_strd_imm(e, PSX_R3, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_HI);

    uint16_t *done = psx_emit_b_short_fwd(e);

    psx_emit_patch_bcond_short(e, zero, PSX_CC_EQ, psx_emit_here(e));

    /* divisor zero: hi = numerator, lo = 0xffffffff, or +1 when the signed
       numerator is negative */
    psx_emit_mvn_imm12(e, PSX_R2, 0);

    if (is_signed)
    {
        psx_emit_cmp_imm8(e, PSX_R0, 0);
        psx_emit_it(e, PSX_CC_LT);
        psx_emit_mov_imm8(e, PSX_R2, 1);
    }

    psx_emit_strd_imm(e, PSX_R0, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_HI);

    psx_emit_patch_b_short(e, done, psx_emit_here(e));

    return 1;
}

/* ------------------------------------------------------------------ branches */

/* Can the delay slot of a branch be translated natively? It is emitted after
   the branch has chosen where to go (r3), so it must leave r3 alone and must
   not need the interpreter. */
static inline int psx_jit_delay_ok(uint32_t op, int *is_mem, int *is_trap)
{
    *is_mem = 0;
    *is_trap = 0;

    if (psx_jit_is_trap_alu(op))
    {
        *is_trap = 1;
        return 1;
    }

    if (psx_jit_mem_supported(op))
    {
        *is_mem = 1;
        return 1;
    }

    const uint32_t o = PSX_OP(op);

    if (o == 0x00)
    {
        const uint32_t fn = PSX_FN(op);

        /* the ALU group of translate_alu, without the divide (it branches) */
        return (fn == 0x00) || (fn == 0x02) || (fn == 0x03) || (fn == 0x04) || (fn == 0x06) || (fn == 0x07) ||
               ((fn >= 0x10) && (fn <= 0x13)) || (fn == 0x18) || (fn == 0x19) || ((fn >= 0x21) && (fn <= 0x27)) ||
               (fn == 0x2a) || (fn == 0x2b);
    }

    if ((o >= 0x09) && (o <= 0x0f))
        return 1;

    /* LWC2 / SWC2: the fast path uses r0..r2, the slow one is the interpreter */
    if ((o == 0x32) || (o == 0x3a))
        return 1;

    /* RFE (JR k0 ; RFE ends every interrupt handler): the block then leaves
       for the dispatcher, which sees the interrupt state */
    if (psx_jit_is_rfe(op))
        return 1;

    /* GTE moves and commands: calls keep r3 in r11 */
    if (o == 0x12)
    {
        if (op & 0x02000000u)
            return 1;

        const uint32_t rs = PSX_RS(op);

        return (rs == 0x00) || (rs == 0x02) || (rs == 0x04) || (rs == 0x06);
    }

    return 0;
}

/*
    Branches and jumps, translated together with their delay slot.

    The branch is decided first - where to go ends up in r3 - and the delay slot
    runs after that, before the block leaves. Anything that cannot run there
    natively leaves the whole pair to the interpreter.

    Returns non zero when the pair was translated; the block always ends there.
*/
static inline int psx_jit_translate_branch(psx_jit_ctx_t *c, uint32_t op, uint32_t delay_op)
{
    psx_emit_t *const e = c->e;

    const uint32_t o = PSX_OP(op);
    const uint32_t rs = PSX_RS(op);
    const uint32_t rt = PSX_RT(op);
    const uint32_t fn = PSX_FN(op);

    const uint32_t fall = c->guest + 8u;
    const uint32_t target = c->guest + 4u + (PSX_SIMM(op) << 2);

    uint32_t cond = 0xffffffffu; /* ARM condition of "branch taken" */
    int indirect = 0;            /* target comes from a register */
    int link = 0;                /* writes a return address */
    uint32_t link_reg = 31;
    uint32_t jump_target = 0;    /* constant target of J / JAL */

    switch (o)
    {
    case 0x00:
        if (fn == 0x08) /* JR */
        {
            indirect = 1;
        }
        else if (fn == 0x09) /* JALR */
        {
            indirect = 1;
            link = 1;
            link_reg = PSX_RD(op);
        }
        else
        {
            return 0;
        }
        break;

    case 0x01: /* REGIMM: BLTZ / BGEZ / BLTZAL / BGEZAL */
        switch (rt)
        {
        case 0x00: cond = PSX_CC_LT; break;
        case 0x01: cond = PSX_CC_GE; break;
        case 0x10: cond = PSX_CC_LT; link = 1; break;
        case 0x11: cond = PSX_CC_GE; link = 1; break;
        default: return 0; /* the odd encodings keep the interpreter */
        }
        break;

    case 0x02: /* J */
        jump_target = ((c->guest + 8u) & 0xf0000000u) | ((op & 0x03ffffffu) << 2);
        break;

    case 0x03: /* JAL */
        jump_target = ((c->guest + 8u) & 0xf0000000u) | ((op & 0x03ffffffu) << 2);
        link = 1;
        break;

    case 0x04: cond = PSX_CC_EQ; break; /* BEQ  */
    case 0x05: cond = PSX_CC_NE; break; /* BNE  */
    case 0x06: cond = PSX_CC_LE; break; /* BLEZ */
    case 0x07: cond = PSX_CC_GT; break; /* BGTZ */

    default:
        return 0;
    }

    /* A condition both of whose inputs are known is no condition */
    if ((cond != 0xffffffffu) && psx_jit_is_const(c, rs) && ((o != 0x04 && o != 0x05) || psx_jit_is_const(c, rt)))
    {
        const int32_t s = (int32_t)psx_jit_const_of(c, rs);
        const int32_t t = (int32_t)psx_jit_const_of(c, rt);
        int taken;

        switch (o)
        {
        case 0x04: taken = (s == t); break;
        case 0x05: taken = (s != t); break;
        case 0x06: taken = (s <= 0); break;
        case 0x07: taken = (s > 0); break;
        default: taken = (rt & 1u) ? (s >= 0) : (s < 0); break;
        }

        jump_target = taken ? target : fall;
        cond = 0xffffffffu;
    }

    int delay_is_mem = 0;
    int delay_is_trap = 0;

    if (!psx_jit_delay_ok(delay_op, &delay_is_mem, &delay_is_trap))
        return 0;

    uint32_t mem_flags = PSX_JIT_MEM_DELAY;

    /* A load here would normally have to be left pending, which costs the
       next step an interpreted instruction. When the branch target is known
       the instruction that will observe the load can be read right now, and
       if neither successor touches the register the value can be written
       straight away. MFC2 / CFC2 are loads too. */
    const int delay_loads = (delay_is_mem && (PSX_OP(delay_op) < 0x28u)) ||
                            ((PSX_OP(delay_op) == 0x12u) && !(delay_op & 0x02000000u) &&
                             ((PSX_RS(delay_op) == 0x00u) || (PSX_RS(delay_op) == 0x02u)) && PSX_RT(delay_op));

    if (delay_loads)
    {
        const uint32_t drt = PSX_RT(delay_op);

        int conflict = 1;

        const uint32_t after = (cond == 0xffffffffu) ? jump_target : target;

        /* only look at code in guest RAM: reading anything else at compile
           time could touch a device register */
        if (!indirect && ((after & 0x1fffffffu) < 0x00200000u) && ((fall & 0x1fffffffu) < 0x00200000u))
        {
            const uint32_t op_after = c->read32(c->ud, after);

            conflict = psx_jit_reads_reg(op_after, drt) || psx_jit_writes_reg(op_after, drt);

            if (!conflict && (cond != 0xffffffffu))
            {
                const uint32_t op_fall = c->read32(c->ud, fall);

                conflict = psx_jit_reads_reg(op_fall, drt) || psx_jit_writes_reg(op_fall, drt);
            }
        }

        if (conflict)
            mem_flags |= PSX_JIT_MEM_PENDING;
    }

    /*
        Where the block goes next travels in r3 across the delay slot. For a
        target known at compile time that is the link of the block there, so the
        end of this block can jump straight into it; a register target goes
        through the jump helper; a load left pending (which the next instruction
        has to see through the interpreter) is the plain pc and goes back to the
        dispatcher.
    */
    uint32_t go_taken = (cond == 0xffffffffu) ? jump_target : target;
    uint32_t go_fall = fall;

    c->exit_mode = indirect ? PSX_JIT_EXIT_JR : PSX_JIT_EXIT_PC;

    if (!indirect && !(mem_flags & PSX_JIT_MEM_PENDING) && !psx_jit_is_rfe(delay_op) && c->get_link)
    {
        const uint32_t link_taken = c->get_link(c->ud, go_taken);
        const uint32_t link_fall = (cond == 0xffffffffu) ? link_taken : c->get_link(c->ud, go_fall);

        if (link_taken && link_fall)
        {
            go_taken = link_taken - 1u;
            go_fall = link_fall - 1u;

            c->exit_mode = PSX_JIT_EXIT_LINK;
        }
    }

    if ((indirect && (mem_flags & PSX_JIT_MEM_PENDING)) || psx_jit_is_rfe(delay_op))
        c->exit_mode = PSX_JIT_EXIT_PC;

    const int is_link = (c->exit_mode == PSX_JIT_EXIT_LINK);

    if (indirect)
    {
        psx_jit_ld_reg(e, PSX_R3, rs); /* read rs before the link register is written */
    }
    else if (cond == 0xffffffffu)
    {
        psx_jit_r3_const(e, go_taken, is_link);
    }
    else
    {
        psx_jit_ld_reg(e, PSX_R0, rs);

        if ((o == 0x04) || (o == 0x05)) /* BEQ / BNE compare two registers */
        {
            psx_jit_ld_reg(e, PSX_R1, rt);
            psx_emit_cmp_reg(e, PSX_R0, PSX_R1);
        }
        else
        {
            psx_emit_cmp_imm8(e, PSX_R0, 0);
        }

        psx_jit_r3_const(e, go_fall, is_link);

        if (is_link)
            psx_emit_it(e, cond);
        else
            psx_emit_itt(e, cond);

        psx_jit_r3_const(e, go_taken, is_link);
    }

    /* the return address is written whether or not the branch is taken */
    if (link)
    {
        psx_jit_st_const(c, link_reg, fall);
    }

    /* the delay slot; whoever called commits what r3 says once it is through */
    psx_jit_ctx_t dc = *c;

    dc.guest = c->guest + 4u;
    dc.next_op = 0xffffffffu; /* the next instruction is the branch target */
    dc.in_delay = 1;
    dc.cycles = c->cycles + 2u; /* the branch's own */
    dc.force_next = 0;
    dc.cycles_done = 0;

    int ok;

    /* the BIOS TTY hook fires when the delay slot at 0xb4 is fetched */
    if (PSX_JIT_IS_BHOOK_PC(dc.guest))
    {
        psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
        psx_jit_call_keep_r3(&dc, PSX_JIT_H_BHOOK);
    }

    if (delay_is_trap)
        ok = psx_jit_translate_trap_alu_ex(&dc, delay_op, 1);
    else if (delay_is_mem)
        ok = psx_jit_translate_mem_ex(&dc, delay_op, mem_flags);
    else if (PSX_OP(delay_op) == 0x12u)
    {
        /* a GTE register load the target sees goes through the load delay
           slot, like a memory load: the conflict test above decided, and the
           move looks at "the next instruction" to know - a NOP touches nothing,
           the unknown instruction everything */
        dc.next_op = (mem_flags & PSX_JIT_MEM_PENDING) ? 0xffffffffu : 0u;

        ok = psx_jit_translate_cop2(&dc, delay_op);
    }
    else if ((PSX_OP(delay_op) == 0x32u) || (PSX_OP(delay_op) == 0x3au))
        ok = psx_jit_translate_gte_mem(&dc, delay_op);
    else if (psx_jit_is_rfe(delay_op))
    {
        psx_jit_emit_rfe(e);
        ok = 1;
    }
    else
        ok = (psx_jit_translate_alu_c(&dc, delay_op) != 0);

    if (!ok)
    {
        e->overflow = 1; /* cannot happen: delay_ok said yes. Never emit it half. */
        return 0;
    }

    /* what the delay slot added: stubs, calls to point at trampolines, what is
       known about the registers */
    c->stub_count = dc.stub_count;
    c->call_count = dc.call_count;
    c->const_mask = dc.const_mask;
    for (uint32_t r = 1; r < 32u; r++)
        c->const_val[r] = dc.const_val[r];

    /* the cycles of the branch and the delay slot, or of the GTE command */
    c->cycles = dc.cycles + (dc.cycles_done ? 0u : 2u);
    c->cycles_done = 1; /* counted here: the block builder adds nothing */

    return 1;
}

/* Instructions after which the interpreter may leave a pending load, so the
   next instruction has to run through the interpreter as well. */
static inline int psx_jit_leaves_pending_load(uint32_t op)
{
    switch (PSX_OP(op))
    {
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x30:
    case 0x31:
    case 0x33:
    case 0x10: /* MFC0 */
    case 0x12: /* MFC2 / CFC2 */
        return 1;

    default:
        return 0;
    }
}

/* Control transfers: the interpreter runs them together with their delay slot */
static inline int psx_jit_is_branch(uint32_t op)
{
    const uint32_t o = PSX_OP(op);

    if ((o == 0x02) || (o == 0x03) || (o == 0x01) || ((o >= 0x04) && (o <= 0x07)))
        return 1;

    if (o == 0x00)
    {
        const uint32_t fn = PSX_FN(op);

        if ((fn == 0x08) || (fn == 0x09) || (fn == 0x0c) || (fn == 0x0d))
            return 1;
    }

    if (o == 0x10) /* COP0, includes RFE */
        return 1;

    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
