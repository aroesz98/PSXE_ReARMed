#ifndef PSX_JIT_TRANSLATE_H
#define PSX_JIT_TRANSLATE_H

/*
    Guest instruction -> Thumb-2 translation.

    Anything that can trap, that changes control flow, or whose semantics depend
    on state the translator does not track is handed to the interpreter - one
    single instruction at a time, from inside the same block. That is what keeps
    behaviour identical to the pure interpreter while the hot arithmetic and the
    RAM accesses run as native code.

    Host register usage inside a block:
      r0..r3  scratch / helper arguments (r3: where the block goes next)
      r4      psx_cpu_t *
      r5      emulated cycles of the natively translated instructions
      r6      base of the guest RAM buffer
      r7      base of the "this page holds translated code" table
      r8      cycle budget: a block only hands over to the next one below it
      r9      table of helper entry points
      r10     where a block returns to (the dispatcher's entry stub)

    A block has no prologue or epilogue of its own - the entry stub sets the
    registers up once and every block leaves through r10 - and it holds no
    address that depends on where it sits: branches stay inside the block,
    helpers are called through r9 and other blocks are reached through their
    link. That makes a block a plain run of bytes that can be copied between the
    code tiers.
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
#define PSX_JIT_BUDGET PSX_R8
#define PSX_JIT_HELPERS PSX_R9
#define PSX_JIT_EXIT PSX_R10

/* slots of the helper table, as byte offsets from r9 */
#define PSX_JIT_H_INTERP 0u     /* uint32_t f(cpu)              run one instruction       */
#define PSX_JIT_H_INTERP_AT 4u  /* uint32_t f(cpu, pc)          ... after publishing pc   */
#define PSX_JIT_H_LOAD_AT 8u    /* uint32_t f(cpu, pc)          ... and apply the load    */
#define PSX_JIT_H_DELAY 12u     /* void f(cpu, pc, next_pc)     run a branch delay slot   */
#define PSX_JIT_H_GTE 16u       /* int32_t f(cpu, opcode)       GTE command               */
#define PSX_JIT_H_GTE_READ 20u  /* uint32_t f(cpu, reg)         MFC2 / CFC2               */
#define PSX_JIT_H_GTE_WRITE 24u /* void f(cpu, reg, value)      MTC2 / CTC2               */
#define PSX_JIT_H_GTE_MEM 28u   /* uint32_t f(cpu, pc, opcode)  LWC2 / SWC2               */
#define PSX_JIT_H_MEM 32u       /* uint32_t f(cpu, pc, addr, d) access that is not RAM    */
#define PSX_JIT_H_COUNT 9u

/*
    What PSX_JIT_H_MEM is told about the access, so it does not have to fetch and
    decode the instruction: the scratchpad is served right there, everything else
    goes to the interpreter as before.
*/
#define PSX_JIT_MD_RT(d) ((d) & 0x1fu)
#define PSX_JIT_MD_SIZE(d) (1u << (((d) >> 5) & 3u))
#define PSX_JIT_MD_SIGNED 0x080u
#define PSX_JIT_MD_STORE 0x100u
#define PSX_JIT_MD_PENDING 0x200u /* a load: leave it in the load delay slot */

/*
    A link is how blocks refer to each other: the entry point of the block at a
    guest address. It exists from the moment some block branches to that address,
    before anything has been compiled for it - until then (and again after the
    block has been invalidated) the entry point is the dispatcher's, so jumping
    through a link is always safe. Moving a block between the code tiers only has
    to update its one link.
*/
typedef struct
{
    uint32_t code; /* host entry point (Thumb) */
    uint32_t pc;   /* guest address           */
} psx_jit_link_t;

#define PSX_JIT_LINK_CODE 0u
#define PSX_JIT_LINK_PC 4u

/* what a block ending branch leaves in r3 */
#define PSX_JIT_EXIT_PC 0   /* the guest pc: the block returns to the dispatcher */
#define PSX_JIT_EXIT_LINK 1 /* the link of the next block: it may be run directly */

/* Guest RAM window test: bits 28:21 are zero exactly for the three RAM mirrors
   (KUSEG / KSEG0 / KSEG1, low 2 MB). Scratchpad, I/O, BIOS and the cache
   control register all fail it and take the interpreter path. */
#define PSX_JIT_RAM_WINDOW_MASK 0x1fe00000u

/* offsets inside psx_cpu_t */
#define PSX_JIT_OFF_R(n) ((uint32_t)(offsetof(psx_cpu_t, r) + (n) * 4u))
#define PSX_JIT_OFF_PC ((uint32_t)offsetof(psx_cpu_t, pc))
#define PSX_JIT_OFF_NEXT_PC ((uint32_t)offsetof(psx_cpu_t, next_pc))
#define PSX_JIT_OFF_HI ((uint32_t)offsetof(psx_cpu_t, hi))
#define PSX_JIT_OFF_LO ((uint32_t)offsetof(psx_cpu_t, lo))

/* guest instruction fields */
#define PSX_OP(op) ((op) >> 26)
#define PSX_RS(op) (((op) >> 21) & 0x1fu)
#define PSX_RT(op) (((op) >> 16) & 0x1fu)
#define PSX_RD(op) (((op) >> 11) & 0x1fu)
#define PSX_SA(op) (((op) >> 6) & 0x1fu)
#define PSX_FN(op) ((op) & 0x3fu)
#define PSX_IMM(op) ((op) & 0xffffu)
#define PSX_SIMM(op) ((uint32_t)(int32_t)(int16_t)((op) & 0xffffu))

/* Translation context for one instruction */
typedef struct
{
    psx_emit_t *e;
    uint32_t guest;         /* address of the instruction being translated */
    uint32_t next_op;       /* the instruction that follows it             */
    uint16_t **exits;       /* branches that leave the block               */
    uint32_t *exit_count;
    uint32_t exit_max;
    int force_next;         /* the next instruction must be interpreted    */
    int cycles_done;        /* the instruction accounted for its own cycles*/
    int exit_mode;          /* PSX_JIT_EXIT_*: what the ending branch left in r3 */

    /* guest code, to look at the instructions a branch leads to */
    uint32_t (*read32)(void *ud, uint32_t addr);

    /* address of the link for the block at pc, 0 when none can be had */
    uint32_t (*get_link)(void *ud, uint32_t pc);

    void *ud;
} psx_jit_ctx_t;

/* offsets of the load delay slot, written by natively translated loads whose
   result must not become visible yet */
#define PSX_JIT_OFF_LOAD_D ((uint32_t)offsetof(psx_cpu_t, load_d))
#define PSX_JIT_OFF_LOAD_V ((uint32_t)offsetof(psx_cpu_t, load_v))
#define PSX_JIT_OFF_BRANCH ((uint32_t)offsetof(psx_cpu_t, branch))

/* translate_mem modes */
#define PSX_JIT_MEM_DELAY 1u   /* the access sits in a branch delay slot      */
#define PSX_JIT_MEM_PENDING 2u /* a load leaves its value in the delay slot   */

static inline void psx_jit_add_exit(psx_jit_ctx_t *c, uint16_t *slot)
{
    if (*c->exit_count < c->exit_max)
    {
        c->exits[(*c->exit_count)++] = slot;
        return;
    }

    /* An unpatched exit would fall through and keep running the block with a pc
       that no longer matches, so the block is abandoned instead. */
    c->e->overflow = 1;
}

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

/* Publishes the guest pc so the interpreter can take over at this instruction */
static inline void psx_jit_publish_pc(psx_emit_t *e, uint32_t guest)
{
    psx_emit_imm32(e, PSX_R0, guest);
    psx_emit_add_imm12(e, PSX_R1, PSX_R0, 4);

    /* pc and next_pc are adjacent, so a single store covers both */
    if ((PSX_JIT_OFF_NEXT_PC == (PSX_JIT_OFF_PC + 4u)) && ((PSX_JIT_OFF_PC & 3u) == 0u) &&
        (PSX_JIT_OFF_PC <= 1020u))
    {
        psx_emit_strd_imm(e, PSX_R0, PSX_R1, PSX_JIT_CPU, PSX_JIT_OFF_PC);
    }
    else
    {
        psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_PC);
        psx_emit_str_imm(e, PSX_R1, PSX_JIT_CPU, PSX_JIT_OFF_NEXT_PC);
    }
}

/* Writes the guest pc that the block leaves behind: r3 holds it, next_pc is
   always one instruction further. */
static inline void psx_jit_commit_pc_reg(psx_emit_t *e)
{
    psx_emit_add_imm12(e, PSX_R0, PSX_R3, 4);

    if ((PSX_JIT_OFF_NEXT_PC == (PSX_JIT_OFF_PC + 4u)) && ((PSX_JIT_OFF_PC & 3u) == 0u) &&
        (PSX_JIT_OFF_PC <= 1020u))
    {
        psx_emit_strd_imm(e, PSX_R3, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_PC);
    }
    else
    {
        psx_emit_str_imm(e, PSX_R3, PSX_JIT_CPU, PSX_JIT_OFF_PC);
        psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_NEXT_PC);
    }
}

/* The end of a block whose r3 holds the link of the next one: publish the pc
   that block starts at - whatever happens next finds the guest state complete -
   and, while the slice still has cycles left, run it without going back to the
   dispatcher. Falls through when the budget is used up. */
static inline void psx_jit_emit_chain(psx_emit_t *e)
{
    psx_emit_ldr_imm(e, PSX_R0, PSX_R3, PSX_JIT_LINK_PC);
    psx_emit_add_imm12(e, PSX_R1, PSX_R0, 4);
    psx_emit_strd_imm(e, PSX_R0, PSX_R1, PSX_JIT_CPU, PSX_JIT_OFF_PC);

    psx_emit_cmp_reg(e, PSX_JIT_CYC, PSX_JIT_BUDGET);
    psx_emit_it(e, PSX_CC_CC); /* unsigned lower */
    psx_emit_ldr_pc(e, PSX_R3, PSX_JIT_LINK_CODE);
}

/* Loads a constant into r3 with a fixed two instruction sequence, so it can sit
   inside an IT block. */
static inline void psx_jit_pc_const(psx_emit_t *e, uint32_t value)
{
    psx_emit_movw(e, PSX_R3, value & 0xffffu);
    psx_emit_movt(e, PSX_R3, value >> 16);
}

/* "Run this one instruction in the interpreter, leave the block if control flow
   diverged." Used for untranslatable instructions and as the escape hatch of
   the native memory fast paths. */
/* Calls a helper through the table in r9. The helpers live in ITCM and OCRAM
   while a block may run from SDRAM, far outside BL range - and a PC relative
   call would not survive the block being copied anyway. */
static inline void psx_jit_emit_call(psx_emit_t *e, uint32_t slot)
{
    psx_emit_ldr_imm(e, PSX_R12, PSX_JIT_HELPERS, slot);
    psx_emit_blx(e, PSX_R12);
}

static inline void psx_jit_emit_interp_call(psx_jit_ctx_t *c, int publish_pc, int is_load)
{
    psx_emit_mov(c->e, PSX_R0, PSX_JIT_CPU);

    /* The helper publishes the pc itself when it is handed one: that is two
       instructions here instead of the four it takes to store pc / next_pc.
       It also compares against cpu->saved_pc, so the block does not have to
       materialise the pc it expects back. */
    if (publish_pc)
    {
        psx_emit_imm32(c->e, PSX_R1, c->guest);
        psx_jit_emit_call(c->e, is_load ? PSX_JIT_H_LOAD_AT : PSX_JIT_H_INTERP_AT);
    }
    else
    {
        psx_jit_emit_call(c->e, PSX_JIT_H_INTERP);
    }

    psx_emit_cmp_imm8(c->e, PSX_R0, 0);

    psx_jit_add_exit(c, psx_emit_bcond_fwd(c->e, PSX_CC_NE));
}

static inline void psx_jit_emit_interp_one(psx_jit_ctx_t *c, int publish_pc)
{
    psx_jit_emit_interp_call(c, publish_pc, 0);
}

/* Escape of an instruction that sits in a branch delay slot: hand it to the
   interpreter with the branch pending. The helper sets pc to the delay slot,
   next_pc to the branch target and the branch flag, so the interpreter runs it
   as a delay slot and leaves pc at the target - or at an exception vector with
   the right EPC and BD bit, which is why this path never hands over to the next
   block. The interpreter accounts for the delay slot's cycles, the branch's are
   added here. c->guest is the delay slot, r3 what the branch left there. */
static inline void psx_jit_emit_delay_escape(psx_jit_ctx_t *c)
{
    psx_emit_t *const e = c->e;

    psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
    psx_emit_imm32(e, PSX_R1, c->guest);

    if (c->exit_mode == PSX_JIT_EXIT_LINK)
        psx_emit_ldr_imm(e, PSX_R2, PSX_R3, PSX_JIT_LINK_PC);
    else
        psx_emit_mov(e, PSX_R2, PSX_R3);

    psx_jit_emit_call(e, PSX_JIT_H_DELAY);

    psx_emit_add_imm12(e, PSX_JIT_CYC, PSX_JIT_CYC, 2);
    psx_emit_bx(e, PSX_JIT_EXIT);
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

        return PSX_RD(op) == r;
    }

    if ((o == 0x03) || (o == 0x01)) /* JAL, BLTZAL / BGEZAL */
        return r == 31u;

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

/* ------------------------------------------------------------------ ALU */

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

            /* r12 takes the high half: r3 may carry the pc of a branch this
               instruction is the delay slot of. */
            if (PSX_FN(op) == 0x18)
                psx_emit_smull(e, PSX_R2, PSX_R12, PSX_R0, PSX_R1);
            else
                psx_emit_umull(e, PSX_R2, PSX_R12, PSX_R0, PSX_R1);

            if ((PSX_JIT_OFF_LO == (PSX_JIT_OFF_HI + 4u)) && ((PSX_JIT_OFF_HI & 3u) == 0u) &&
                (PSX_JIT_OFF_HI <= 1020u))
            {
                psx_emit_strd_imm(e, PSX_R12, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_HI);
            }
            else
            {
                psx_emit_str_imm(e, PSX_R12, PSX_JIT_CPU, PSX_JIT_OFF_HI);
                psx_emit_str_imm(e, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_LO);
            }

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
            if (imm < 0x1000u)
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

        psx_emit_movw(e, PSX_R0, 0);
        psx_emit_movt(e, PSX_R0, PSX_IMM(op));
        psx_jit_st_reg(e, PSX_R0, rt);

        return 1;
    }

    default:
        return 0;
    }
}

/* ------------------------------------------------------- trapping arithmetic */

/*
    ADD, ADDI and SUB raise an overflow exception, so the native form keeps the
    flags and escapes to the interpreter when V is set - the interpreter is what
    raises the exception, with the correct EPC.
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

    uint16_t *overflow = psx_emit_bcond_short_fwd(e, PSX_CC_VS);

    psx_jit_st_reg(e, PSX_R0, dst);

    uint16_t *done = psx_emit_b_short_fwd(e);

    psx_emit_patch_bcond_short(e, overflow, PSX_CC_VS, psx_emit_here(e));

    if (in_delay)
        psx_jit_emit_delay_escape(c);
    else
        psx_jit_emit_interp_one(c, 1);

    psx_emit_patch_b_short(e, done, psx_emit_here(e));

    return 1;
}

static inline int psx_jit_translate_trap_alu(psx_jit_ctx_t *c, uint32_t op)
{
    return psx_jit_translate_trap_alu_ex(c, op, 0);
}

/* --------------------------------------------------------------- GTE */

/*
    GTE commands become one call. The helper returns the cycle count, which is
    why the block adds r0 to the cycle counter instead of carrying a copy of the
    timing table.

    The register moves are a call each as well. MTC2 / CTC2 are plain writes.
    MFC2 / CFC2 are loads, with the load delay of one: like a memory load, the
    value goes straight into the guest register when the next instruction does
    not touch it, and into the load delay slot - with the next instruction
    interpreted, which applies it - when it does.
*/
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
            psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
            psx_emit_mov_imm8(e, PSX_R1, rd + ((PSX_RS(op) == 0x06) ? 32u : 0u));
            psx_jit_ld_reg(e, PSX_R2, rt);
            psx_jit_emit_call(e, PSX_JIT_H_GTE_WRITE);

            return 1;
        }

        case 0x00: /* MFC2 */
        case 0x02: /* CFC2 */
        {
            psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
            psx_emit_mov_imm8(e, PSX_R1, rd + ((PSX_RS(op) == 0x02) ? 32u : 0u));
            psx_jit_emit_call(e, PSX_JIT_H_GTE_READ);

            if (rt == 0)
                return 1; /* the read has no side effects and nowhere to go */

            if (psx_jit_reads_reg(c->next_op, rt) || psx_jit_writes_reg(c->next_op, rt))
            {
                psx_emit_mov_imm8(e, PSX_R1, rt);

                if ((PSX_JIT_OFF_LOAD_V == (PSX_JIT_OFF_LOAD_D + 4u)) &&
                    ((PSX_JIT_OFF_LOAD_D & 3u) == 0u) && (PSX_JIT_OFF_LOAD_D <= 1020u))
                {
                    psx_emit_strd_imm(e, PSX_R1, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_D);
                }
                else
                {
                    psx_emit_str_imm(e, PSX_R1, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_D);
                    psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_V);
                }

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
    psx_jit_emit_call(e, PSX_JIT_H_GTE);
    psx_emit_add_reg(e, PSX_JIT_CYC, PSX_JIT_CYC, PSX_R0);

    c->cycles_done = 1;

    return 1;
}

/* LWC2 / SWC2: one call into the interpreter's handlers, see psx_cpu_gte_transfer.
   The helper accounts for the cycles itself. */
static inline int psx_jit_translate_gte_mem(psx_jit_ctx_t *c, uint32_t op)
{
    if ((PSX_OP(op) != 0x32u) && (PSX_OP(op) != 0x3au))
        return 0;

    psx_emit_t *const e = c->e;

    psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
    psx_emit_imm32(e, PSX_R1, c->guest);
    psx_emit_imm32(e, PSX_R2, op);
    psx_jit_emit_call(e, PSX_JIT_H_GTE_MEM);

    psx_emit_cmp_imm8(e, PSX_R0, 0);

    psx_jit_add_exit(c, psx_emit_bcond_fwd(e, PSX_CC_NE));

    c->cycles_done = 1;

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

/* ------------------------------------------------------------------ memory */

/* Emits r0 = guest address, r1 = offset inside the RAM buffer, and collects the
   branches that have to be pointed at the escape path: one for "not guest RAM"
   and, for halfword / word accesses, one for "misaligned" - a misaligned access
   has to raise an address error, which only the interpreter does. The two tests
   are separate because the combined mask is not a Thumb-2 modified immediate. */
static inline void psx_jit_emit_addr(psx_emit_t *e, uint32_t rs, uint32_t simm, uint32_t align_mask,
                                     uint16_t **slots, uint32_t *slot_count)
{
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

    /* Both tests feed one branch: ANDS sets Z when the address is inside the
       guest RAM window, and the conditional TST folds the alignment check into
       the same flag. Anything else escapes to the interpreter, which also
       raises the address error of a misaligned access. */
    psx_emit_ands_imm12(e, PSX_R2, PSX_R0, psx_thumb_expand_imm(PSX_JIT_RAM_WINDOW_MASK));

    if (align_mask)
    {
        psx_emit_it(e, PSX_CC_EQ);
        psx_emit_tst_imm12(e, PSX_R0, align_mask);
    }

    slots[(*slot_count)++] = psx_emit_bcond_short_fwd(e, PSX_CC_NE);

    psx_emit_bic_imm12(e, PSX_R1, PSX_R0, psx_thumb_expand_imm(0xe0000000u));
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

/* LB / LBU / LH / LHU / LW / SB / SH / SW against guest RAM */
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

    uint16_t *escapes[3];
    uint32_t escape_count = 0;

    psx_jit_emit_addr(e, rs, simm, size - 1u, escapes, &escape_count);

    if (is_load)
    {
        if (size == 4)
            psx_emit_ldr_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
        else if (size == 2)
        {
            if (is_signed)
                psx_emit_ldrsh_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
            else
                psx_emit_ldrh_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
        }
        else
        {
            if (is_signed)
                psx_emit_ldrsb_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
            else
                psx_emit_ldrb_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
        }

        if (flags & PSX_JIT_MEM_PENDING)
        {
            /* The value must not be visible to the next instruction, so it goes
               into the load delay slot exactly like the interpreter would leave
               it; the next instruction is interpreted and applies it. */
            psx_emit_mov_imm8(e, PSX_R1, rt);

            if ((PSX_JIT_OFF_LOAD_V == (PSX_JIT_OFF_LOAD_D + 4u)) &&
                ((PSX_JIT_OFF_LOAD_D & 3u) == 0u) && (PSX_JIT_OFF_LOAD_D <= 1020u))
            {
                psx_emit_strd_imm(e, PSX_R1, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_D);
            }
            else
            {
                psx_emit_str_imm(e, PSX_R1, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_D);
                psx_emit_str_imm(e, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_LOAD_V);
            }
        }
        else
        {
            psx_jit_st_reg(e, PSX_R2, rt);
        }
    }
    else
    {
        psx_jit_ld_reg(e, PSX_R2, rt);

        /* A store into a page some block was translated from has to invalidate
           it, and the generic write path is what does that. Inside a delay slot
           r3 says where the block goes next, so the scratch is r12 there - which
           costs the wide encodings; everywhere else r3 is free. */
        const uint32_t tmp = (flags & PSX_JIT_MEM_DELAY) ? PSX_R12 : PSX_R3;

        psx_emit_shift_imm(e, 1, tmp, PSX_R1, 10); /* 1 KB page index */
        psx_emit_ldrb_reg(e, tmp, PSX_JIT_PAGES, tmp);

        if (PSX_EMIT_LOW(tmp))
            psx_emit_cmp_imm8(e, tmp, 0);
        else
            psx_emit_cmp_imm12(e, tmp, 0);

        escapes[escape_count++] = psx_emit_bcond_short_fwd(e, PSX_CC_NE);

        if (size == 4)
            psx_emit_str_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
        else if (size == 2)
            psx_emit_strh_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
        else
            psx_emit_strb_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
    }

    /* fast path done: jump over the escape code. In a delay slot that leads to
       the end of the block, where the pc is committed; the escape ends the block
       itself. */
    uint16_t *done = psx_emit_b_short_fwd(e);

    const uint32_t escape_here = psx_emit_here(e);

    for (uint32_t i = 0; i < escape_count; i++)
        psx_emit_patch_bcond_short(e, escapes[i], PSX_CC_NE, escape_here);

    if (flags & PSX_JIT_MEM_DELAY)
    {
        psx_jit_emit_delay_escape(c);
    }
    else
    {
        /* A load handed to the interpreter leaves its result in the load delay
           slot, and native code never applies a pending load - so unless the
           block wants it pending anyway, the escape uses the helper that
           applies it before returning. */
        /* r0 still holds the guest address: both escapes leave it alone */
        const uint32_t desc = rt | ((size == 4u) ? 0x40u : ((size == 2u) ? 0x20u : 0u)) |
                              (is_signed ? PSX_JIT_MD_SIGNED : 0u) | (is_load ? 0u : PSX_JIT_MD_STORE) |
                              ((flags & PSX_JIT_MEM_PENDING) ? PSX_JIT_MD_PENDING : 0u);

        psx_emit_mov(e, PSX_R2, PSX_R0);
        psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
        psx_emit_imm32(e, PSX_R1, c->guest);
        psx_emit_movw(e, PSX_R3, desc);
        psx_jit_emit_call(e, PSX_JIT_H_MEM);

        psx_emit_cmp_imm8(e, PSX_R0, 0);

        psx_jit_add_exit(c, psx_emit_bcond_fwd(e, PSX_CC_NE));
    }

    psx_emit_patch_b_short(e, done, psx_emit_here(e));

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

/* ------------------------------------------------------------------ branches */

/*
    Branches and jumps, translated together with their delay slot.

    The delay slot instruction runs after the condition has been evaluated but
    before the block leaves, so it is translated into a scratch buffer first:
    when it is not plain arithmetic (a load, a store, a coprocessor access -
    anything that can trap or that carries a load delay across the branch) the
    whole pair is left to the interpreter. Arithmetic emits no branches and no
    absolute addresses, so copying it is safe.

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

    /* Classify the delay slot before anything is emitted. Arithmetic is
       translated into a scratch buffer and copied in (it contains no branches
       and no absolute addresses); a memory access has to be emitted in place,
       because its escape path branches. */
    uint16_t scratch[32];
    psx_emit_t se;
    int delay_is_mem = 0;
    uint32_t mem_flags = PSX_JIT_MEM_DELAY;

    psx_emit_init(&se, scratch, (uint32_t)sizeof(scratch));

    int delay_is_trap = 0;

    if (!psx_jit_translate_alu(&se, delay_op) || se.overflow)
    {
        /* ADD / ADDI / SUB: hand written code likes ADDI in delay slots, and
           leaving those pairs to the interpreter was most of what it still ran
           during full motion video */
        delay_is_trap = psx_jit_is_trap_alu(delay_op);

        if (!delay_is_trap && !psx_jit_mem_supported(delay_op))
            return 0;

        delay_is_mem = !delay_is_trap;
    }

    if (delay_is_mem)
    {

        /* A load here would normally have to be left pending, which costs the
           next step an interpreted instruction. When the branch target is known
           the instruction that will observe the load can be read right now, and
           if neither successor touches the register the value can be written
           straight away. */
        if (PSX_OP(delay_op) < 0x28u)
        {
            const uint32_t drt = PSX_RT(delay_op);

            int conflict = 1;

            const uint32_t after = (cond == 0xffffffffu) ? jump_target : target;

            /* only look at code in guest RAM: reading anything else at compile
               time could touch a device register */
            if (!indirect && ((after & 0x1fffffffu) < 0x00200000u) &&
                ((fall & 0x1fffffffu) < 0x00200000u))
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
    }

    /*
        Where the block goes next travels in r3 across the delay slot. For a
        target known at compile time that is the link of the block there, so the
        end of this block can jump straight into it; a register target, or a
        load left pending (which the next instruction has to see through the
        interpreter), is the plain pc and goes back to the dispatcher.
    */
    uint32_t go_taken = (cond == 0xffffffffu) ? jump_target : target;
    uint32_t go_fall = fall;

    c->exit_mode = PSX_JIT_EXIT_PC;

    if (!indirect && !(mem_flags & PSX_JIT_MEM_PENDING) && c->get_link)
    {
        const uint32_t link_taken = c->get_link(c->ud, go_taken);
        const uint32_t link_fall = (cond == 0xffffffffu) ? link_taken : c->get_link(c->ud, go_fall);

        if (link_taken && link_fall)
        {
            go_taken = link_taken;
            go_fall = link_fall;

            c->exit_mode = PSX_JIT_EXIT_LINK;
        }
    }

    if (indirect)
    {
        psx_jit_ld_reg(e, PSX_R3, rs); /* read rs before the link register is written */
    }
    else if (cond == 0xffffffffu)
    {
        psx_jit_pc_const(e, go_taken);
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

        psx_jit_pc_const(e, go_fall);
        psx_emit_itt(e, cond);
        psx_jit_pc_const(e, go_taken);
    }

    /* the return address is written whether or not the branch is taken */
    if (link)
    {
        psx_emit_imm32(e, PSX_R0, fall);
        psx_jit_st_reg(e, PSX_R0, link_reg);
    }

    /* delay slot; whoever called commits what r3 says once it is through */
    if (delay_is_trap)
    {
        psx_jit_ctx_t dc = *c;

        dc.guest = c->guest + 4u;

        if (!psx_jit_translate_trap_alu_ex(&dc, delay_op, 1))
            return 0; /* cannot happen: is_trap_alu already said yes */

        *c->exit_count = *dc.exit_count;

        return 1;
    }

    if (delay_is_mem)
    {
        /* the access does not touch r3, and lays out its own escape */
        psx_jit_ctx_t dc = *c;

        dc.guest = c->guest + 4u;
        dc.next_op = 0xffffffffu; /* the next instruction is the branch target */

        if (!psx_jit_translate_mem_ex(&dc, delay_op, mem_flags))
            return 0; /* cannot happen: mem_supported already said yes */

        *c->exit_count = *dc.exit_count;

        return 1;
    }

    const uint32_t half_words = psx_emit_size(&se) / 2u;

    for (uint32_t i = 0; i < half_words; i++)
        psx_emit16(e, scratch[i]);

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
