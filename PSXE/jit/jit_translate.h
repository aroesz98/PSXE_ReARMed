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
      r0..r3  scratch / helper arguments
      r4      psx_cpu_t *
      r5      emulated cycles of the natively translated instructions
      r6      base of the guest RAM buffer
      r7      base of the "this page holds translated code" table
*/

#include <stdint.h>
#include <stddef.h>

#include "jit_emit.h"
#include "../cpu.h"
#include "../bus_fast.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_JIT_CPU PSX_R4
#define PSX_JIT_CYC PSX_R5
#define PSX_JIT_RAM PSX_R6
#define PSX_JIT_PAGES PSX_R7

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
    uint32_t helper_interp; /* psx_jit_interp_op                           */
    uint32_t helper_mem;    /* psx_jit_interp_load                         */
    uint16_t **exits;       /* branches that leave the block               */
    uint32_t *exit_count;
    uint32_t exit_max;
    int force_next;         /* the next instruction must be interpreted    */
    int cycles_done;        /* the instruction accounted for its own cycles*/
    uint32_t helper_gte;    /* psx_cpu_gte_command                         */
    psx_bus_t *bus;         /* to read the instructions a branch leads to  */
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
static inline void psx_jit_emit_interp_call(psx_jit_ctx_t *c, int publish_pc, uint32_t helper)
{
    if (publish_pc)
        psx_jit_publish_pc(c->e, c->guest);

    /* The helper compares against cpu->saved_pc itself, so the block does not
       have to materialise the expected pc. */
    psx_emit_mov(c->e, PSX_R0, PSX_JIT_CPU);
    psx_emit_bl(c->e, helper);
    psx_emit_cmp_imm8(c->e, PSX_R0, 0);

    psx_jit_add_exit(c, psx_emit_bcond_fwd(c->e, PSX_CC_NE));
}

static inline void psx_jit_emit_interp_one(psx_jit_ctx_t *c, int publish_pc)
{
    psx_jit_emit_interp_call(c, publish_pc, c->helper_interp);
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
static inline int psx_jit_translate_trap_alu(psx_jit_ctx_t *c, uint32_t op)
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

    uint16_t *overflow = psx_emit_bcond_fwd(e, PSX_CC_VS);

    psx_jit_st_reg(e, PSX_R0, dst);

    uint16_t *done = psx_emit_b_fwd(e);

    psx_emit_patch_bcond(overflow, PSX_CC_VS, psx_emit_here(e));

    psx_jit_emit_interp_one(c, 1);

    psx_emit_patch_b(done, psx_emit_here(e));

    return 1;
}

/* --------------------------------------------------------------- GTE */

/*
    GTE commands become one indirect call. The helper lives in OCRAM, which is
    far outside BL range from the ITCM code cache, so the call goes through r12;
    it also returns the cycle count, which is why the block adds r0 to the cycle
    counter instead of carrying a copy of the timing table.

    Everything else on COP2 (MFC2 / CFC2 / MTC2 / CTC2) keeps the interpreter:
    those carry a load delay.
*/
static inline int psx_jit_translate_cop2(psx_jit_ctx_t *c, uint32_t op)
{
    if ((PSX_OP(op) != 0x12) || !(op & 0x02000000u))
        return 0;

    psx_emit_t *const e = c->e;

    psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
    psx_emit_imm32(e, PSX_R1, op);
    psx_emit_imm32(e, PSX_R12, c->helper_gte);
    psx_emit_blx(e, PSX_R12);
    psx_emit_add_reg(e, PSX_JIT_CYC, PSX_JIT_CYC, PSX_R0);

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

    uint16_t *zero = psx_emit_bcond_fwd(e, PSX_CC_EQ);

    if (is_signed)
        psx_emit_sdiv(e, PSX_R2, PSX_R0, PSX_R1);
    else
        psx_emit_udiv(e, PSX_R2, PSX_R0, PSX_R1);

    psx_emit_mls(e, PSX_R3, PSX_R2, PSX_R1, PSX_R0); /* remainder */

    psx_emit_strd_imm(e, PSX_R3, PSX_R2, PSX_JIT_CPU, PSX_JIT_OFF_HI);

    uint16_t *done = psx_emit_b_fwd(e);

    psx_emit_patch_bcond(zero, PSX_CC_EQ, psx_emit_here(e));

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

    psx_emit_patch_b(done, psx_emit_here(e));

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

    slots[(*slot_count)++] = psx_emit_bcond_fwd(e, PSX_CC_NE);

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
           it, and the generic write path is what does that. r12 is the scratch
           here: inside a delay slot r3 carries the pc the block leaves behind. */
        psx_emit_shift_imm(e, 1, PSX_R12, PSX_R1, 10); /* 1 KB page index */
        psx_emit_ldrb_reg(e, PSX_R12, PSX_JIT_PAGES, PSX_R12);
        psx_emit_cmp_imm12(e, PSX_R12, 0);

        escapes[escape_count++] = psx_emit_bcond_fwd(e, PSX_CC_NE);

        if (size == 4)
            psx_emit_str_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
        else if (size == 2)
            psx_emit_strh_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
        else
            psx_emit_strb_reg(e, PSX_R2, PSX_JIT_RAM, PSX_R1);
    }

    /* In a delay slot the pc the block leaves behind is already in place, so the
       fast path falls through to it and the escape, which ends the block itself,
       is laid out after it. */
    if (flags & PSX_JIT_MEM_DELAY)
        psx_jit_commit_pc_reg(e);

    /* fast path done: jump over the escape code */
    uint16_t *done = psx_emit_b_fwd(e);

    const uint32_t escape_here = psx_emit_here(e);

    for (uint32_t i = 0; i < escape_count; i++)
        psx_emit_patch_bcond(escapes[i], PSX_CC_NE, escape_here);

    if (flags & PSX_JIT_MEM_DELAY)
    {
        /* Hand the delay slot to the interpreter with the branch pending:
           pc is the delay slot, next_pc the branch target in r3 and branch is
           set, so the interpreter runs it as a delay slot and leaves pc at the
           target. The block ends right after, so the result is not checked. */
        psx_emit_imm32(e, PSX_R0, c->guest);
        psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_PC);
        psx_emit_str_imm(e, PSX_R3, PSX_JIT_CPU, PSX_JIT_OFF_NEXT_PC);
        psx_emit_mov_imm8(e, PSX_R0, 1);
        psx_emit_str_imm(e, PSX_R0, PSX_JIT_CPU, PSX_JIT_OFF_BRANCH);

        psx_emit_mov(e, PSX_R0, PSX_JIT_CPU);
        psx_emit_bl(e, c->helper_interp);
    }
    else
    {
        /* A load handed to the interpreter leaves its result in the load delay
           slot, and native code never applies a pending load - so unless the
           block wants it pending anyway, the escape uses the helper that
           applies it before returning. */
        psx_jit_emit_interp_call(c, 1,
                                 (is_load && !(flags & PSX_JIT_MEM_PENDING))
                                     ? c->helper_mem
                                     : c->helper_interp);
    }

    psx_emit_patch_b(done, psx_emit_here(e));

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

    psx_emit_init(&se, scratch, (uint32_t)sizeof(scratch));

    if (!psx_jit_translate_alu(&se, delay_op) || se.overflow)
    {
        if (!psx_jit_mem_supported(delay_op))
            return 0;

        delay_is_mem = 1;
    }

    /* r3 = the pc the block leaves behind */
    if (indirect)
    {
        psx_jit_ld_reg(e, PSX_R3, rs); /* read rs before the link register is written */
    }
    else if (cond == 0xffffffffu)
    {
        psx_jit_pc_const(e, jump_target);
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

        psx_jit_pc_const(e, fall);
        psx_emit_itt(e, cond);
        psx_jit_pc_const(e, target);
    }

    /* the return address is written whether or not the branch is taken */
    if (link)
    {
        psx_emit_imm32(e, PSX_R0, fall);
        psx_jit_st_reg(e, PSX_R0, link_reg);
    }

    /* delay slot */
    if (delay_is_mem)
    {
        /* r3 holds the pc to leave behind and the access does not touch it;
           translate_mem_ex stores it and lays out its own escape. */
        psx_jit_ctx_t dc = *c;

        dc.guest = c->guest + 4u;
        dc.next_op = 0xffffffffu; /* the next instruction is the branch target */

        uint32_t mem_flags = PSX_JIT_MEM_DELAY;

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
                const uint32_t op_after = psx_bus_fast_read32(c->bus, after);

                conflict = psx_jit_reads_reg(op_after, drt) || psx_jit_writes_reg(op_after, drt);

                if (!conflict && (cond != 0xffffffffu))
                {
                    const uint32_t op_fall = psx_bus_fast_read32(c->bus, fall);

                    conflict = psx_jit_reads_reg(op_fall, drt) || psx_jit_writes_reg(op_fall, drt);
                }
            }

            if (conflict)
                mem_flags |= PSX_JIT_MEM_PENDING;
        }

        if (!psx_jit_translate_mem_ex(&dc, delay_op, mem_flags))
            return 0; /* cannot happen: mem_supported already said yes */

        *c->exit_count = *dc.exit_count;

        return 1;
    }

    const uint32_t half_words = psx_emit_size(&se) / 2u;

    for (uint32_t i = 0; i < half_words; i++)
        psx_emit16(e, scratch[i]);

    psx_jit_commit_pc_reg(e);

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
    case 0x32:
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
