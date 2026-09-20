#ifndef PSX_JIT_EMIT_H
#define PSX_JIT_EMIT_H

/*
    Minimal Thumb-2 instruction encoder for the recompiler.

    Everything is a static inline function taking an emit cursor, so the code
    generator stays branch-free C with no allocation. Written so it compiles
    unchanged as C++ (no compound literals, no designated initialisers, explicit
    casts).
*/

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ARM register numbers used by the generated code */
#define PSX_R0 0
#define PSX_R1 1
#define PSX_R2 2
#define PSX_R3 3
#define PSX_R4 4
#define PSX_R5 5
#define PSX_R6 6
#define PSX_R7 7
#define PSX_R8 8
#define PSX_R9 9
#define PSX_R10 10
#define PSX_R11 11
#define PSX_R12 12
#define PSX_SP 13
#define PSX_LR 14
#define PSX_PC 15

/* condition codes */
#define PSX_CC_EQ 0x0
#define PSX_CC_NE 0x1
#define PSX_CC_CS 0x2
#define PSX_CC_CC 0x3
#define PSX_CC_MI 0x4
#define PSX_CC_PL 0x5
#define PSX_CC_VS 0x6
#define PSX_CC_VC 0x7
#define PSX_CC_HI 0x8
#define PSX_CC_LS 0x9
#define PSX_CC_GE 0xa
#define PSX_CC_LT 0xb
#define PSX_CC_GT 0xc
#define PSX_CC_LE 0xd
#define PSX_CC_AL 0xe

typedef struct
{
    uint16_t *cur;   /* next halfword to write */
    uint16_t *start; /* first halfword of this block */
    uint16_t *limit; /* one past the last usable halfword */
    int overflow;    /* set once the cursor would pass the limit */
} psx_emit_t;

static inline void psx_emit_init(psx_emit_t *e, void *buffer, uint32_t size_bytes)
{
    e->cur = (uint16_t *)buffer;
    e->start = e->cur;
    e->limit = (uint16_t *)((uint8_t *)buffer + size_bytes);
    e->overflow = 0;
}

static inline uint32_t psx_emit_size(const psx_emit_t *e)
{
    return (uint32_t)((uint8_t *)e->cur - (uint8_t *)e->start);
}

/* Address of the next instruction, as a callable Thumb pointer */
static inline uint32_t psx_emit_here(const psx_emit_t *e)
{
    return (uint32_t)(uintptr_t)e->cur;
}

static inline void psx_emit16(psx_emit_t *e, uint32_t hw)
{
    if (e->cur >= e->limit)
    {
        e->overflow = 1;
        return;
    }

    *e->cur++ = (uint16_t)hw;
}

static inline void psx_emit32(psx_emit_t *e, uint32_t hw1, uint32_t hw2)
{
    if ((e->cur + 1) >= e->limit)
    {
        e->overflow = 1;
        return;
    }

    *e->cur++ = (uint16_t)hw1;
    *e->cur++ = (uint16_t)hw2;
}

/* ---------------------------------------------------------------- data moves */

/* MOV Rd, Rm */
static inline void psx_emit_mov(psx_emit_t *e, uint32_t rd, uint32_t rm)
{
    psx_emit16(e, 0x4600u | ((rd & 0x8u) << 4) | (rm << 3) | (rd & 0x7u));
}

/* MOVW Rd, #imm16 */
static inline void psx_emit_movw(psx_emit_t *e, uint32_t rd, uint32_t imm16)
{
    const uint32_t imm4 = (imm16 >> 12) & 0xfu;
    const uint32_t i = (imm16 >> 11) & 0x1u;
    const uint32_t imm3 = (imm16 >> 8) & 0x7u;
    const uint32_t imm8 = imm16 & 0xffu;

    psx_emit32(e, 0xf240u | (i << 10) | imm4, (imm3 << 12) | (rd << 8) | imm8);
}

/* MOVT Rd, #imm16 */
static inline void psx_emit_movt(psx_emit_t *e, uint32_t rd, uint32_t imm16)
{
    const uint32_t imm4 = (imm16 >> 12) & 0xfu;
    const uint32_t i = (imm16 >> 11) & 0x1u;
    const uint32_t imm3 = (imm16 >> 8) & 0x7u;
    const uint32_t imm8 = imm16 & 0xffu;

    psx_emit32(e, 0xf2c0u | (i << 10) | imm4, (imm3 << 12) | (rd << 8) | imm8);
}

/* Rd = imm32, using the shortest sequence */
static inline void psx_emit_imm32(psx_emit_t *e, uint32_t rd, uint32_t imm32)
{
    psx_emit_movw(e, rd, imm32 & 0xffffu);

    if (imm32 >> 16)
        psx_emit_movt(e, rd, imm32 >> 16);
}

/* ---------------------------------------------------------------- memory */

/* LDR Rt, [Rn, #imm12] */
static inline void psx_emit_ldr_imm(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t imm12)
{
    psx_emit32(e, 0xf8d0u | rn, (rt << 12) | (imm12 & 0xfffu));
}

/* STR Rt, [Rn, #imm12] */
static inline void psx_emit_str_imm(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t imm12)
{
    psx_emit32(e, 0xf8c0u | rn, (rt << 12) | (imm12 & 0xfffu));
}

/* PUSH {reglist} (32 bit form, any registers) */
static inline void psx_emit_push(psx_emit_t *e, uint32_t reglist)
{
    psx_emit32(e, 0xe92du, reglist);
}

/* POP {reglist} (32 bit form, any registers) */
static inline void psx_emit_pop(psx_emit_t *e, uint32_t reglist)
{
    psx_emit32(e, 0xe8bdu, reglist);
}

/* ---------------------------------------------------------------- arithmetic */

/* CMP Rn, #imm8 */
static inline void psx_emit_cmp_imm8(psx_emit_t *e, uint32_t rn, uint32_t imm8)
{
    psx_emit16(e, 0x2800u | (rn << 8) | (imm8 & 0xffu));
}

/* ADD Rd, Rn, #imm12 (T4, no flags) */
static inline void psx_emit_add_imm12(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf200u | (i << 10) | rn, (imm3 << 12) | (rd << 8) | imm8);
}


/* ---------------------------------------------------------------- data processing */

/* register forms, 32 bit encodings so any register can be used */
static inline void psx_emit_add_reg(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xeb00u | rn, (rd << 8) | rm);
}

static inline void psx_emit_sub_reg(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xeba0u | rn, (rd << 8) | rm);
}

static inline void psx_emit_and_reg(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xea00u | rn, (rd << 8) | rm);
}

static inline void psx_emit_orr_reg(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xea40u | rn, (rd << 8) | rm);
}

static inline void psx_emit_eor_reg(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xea80u | rn, (rd << 8) | rm);
}

/* AND Rd, Rn, #imm8 (ThumbExpandImm with a plain 8 bit value) */
static inline void psx_emit_and_imm(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm8)
{
    psx_emit32(e, 0xf000u | rn, (rd << 8) | (imm8 & 0xffu));
}

/* ADDS / SUBS Rd, Rn, Rm (flags, for the trapping MIPS forms) */
static inline void psx_emit_adds_reg(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xeb10u | rn, (rd << 8) | rm);
}

static inline void psx_emit_subs_reg(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xebb0u | rn, (rd << 8) | rm);
}

/* ADDS Rd, Rn, #modified_imm12 */
static inline void psx_emit_adds_imm12(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf110u | (i << 10) | rn, (imm3 << 12) | (rd << 8) | imm8);
}

/* MVN Rd, Rm */
static inline void psx_emit_mvn_reg(psx_emit_t *e, uint32_t rd, uint32_t rm)
{
    psx_emit32(e, 0xea6fu, (rd << 8) | rm);
}

/* shifts by immediate: type 0 = LSL, 1 = LSR, 2 = ASR */
static inline void psx_emit_shift_imm(psx_emit_t *e, uint32_t type, uint32_t rd, uint32_t rm, uint32_t imm5)
{
    /* LSR #0 / ASR #0 do not exist; MIPS means "shift by nothing", so a plain
       move (LSL #0) is emitted instead. */
    if (imm5 == 0)
        type = 0;

    const uint32_t imm3 = (imm5 >> 2) & 0x7u;
    const uint32_t imm2 = imm5 & 0x3u;

    psx_emit32(e, 0xea4fu, (imm3 << 12) | (rd << 8) | (imm2 << 6) | (type << 4) | rm);
}

/* shifts by register: type 0 = LSL, 1 = LSR, 2 = ASR */
static inline void psx_emit_shift_reg(psx_emit_t *e, uint32_t type, uint32_t rd, uint32_t rn, uint32_t rm)
{
    static const uint32_t op[3] = {0xfa00u, 0xfa20u, 0xfa40u};

    psx_emit32(e, op[type] | rn, 0xf000u | (rd << 8) | rm);
}

/* CMP Rn, Rm (16 bit form, low registers only) */
static inline void psx_emit_cmp_reg(psx_emit_t *e, uint32_t rn, uint32_t rm)
{
    psx_emit16(e, 0x4280u | (rm << 3) | rn);
}

/* MOVS Rd, #imm8 (inside an IT block this is MOV) */
static inline void psx_emit_mov_imm8(psx_emit_t *e, uint32_t rd, uint32_t imm8)
{
    psx_emit16(e, 0x2000u | (rd << 8) | (imm8 & 0xffu));
}

/* IT <cond> (single conditional instruction follows) */
static inline void psx_emit_it(psx_emit_t *e, uint32_t cond)
{
    psx_emit16(e, 0xbf00u | (cond << 4) | 0x8u);
}

/* ITT <cond> (two conditional instructions follow) */
static inline void psx_emit_itt(psx_emit_t *e, uint32_t cond)
{
    /* mask<3> carries the second instruction's condition (same as the first),
       mask<2> terminates the block */
    psx_emit16(e, 0xbf00u | (cond << 4) | ((cond & 1u) << 3) | 0x4u);
}

/* ADD Rd, Rd, #imm12 without flags, 32 bit form */
static inline void psx_emit_addw(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    psx_emit_add_imm12(e, rd, rn, imm12);
}


/* ---------------------------------------------------------------- long multiply and divide */

/* SMULL RdLo, RdHi, Rn, Rm */
static inline void psx_emit_smull(psx_emit_t *e, uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xfb80u | rn, (rdlo << 12) | (rdhi << 8) | rm);
}

/* UMULL RdLo, RdHi, Rn, Rm */
static inline void psx_emit_umull(psx_emit_t *e, uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xfba0u | rn, (rdlo << 12) | (rdhi << 8) | rm);
}

/* SDIV Rd, Rn, Rm */
static inline void psx_emit_sdiv(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xfb90u | rn, 0xf0f0u | (rd << 8) | rm);
}

/* UDIV Rd, Rn, Rm */
static inline void psx_emit_udiv(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xfbb0u | rn, 0xf0f0u | (rd << 8) | rm);
}

/* MLS Rd, Rn, Rm, Ra  (Rd = Ra - Rn * Rm) */
static inline void psx_emit_mls(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra)
{
    psx_emit32(e, 0xfb00u | rn, (ra << 12) | (rd << 8) | 0x10u | rm);
}

/* MVN Rd, #modified_imm12 */
static inline void psx_emit_mvn_imm12(psx_emit_t *e, uint32_t rd, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf06fu | (i << 10), (imm3 << 12) | (rd << 8) | imm8);
}

/* ---------------------------------------------------------------- immediates */

/* Encodes a constant as a Thumb-2 modified immediate (imm12), or returns
   0xffffffff when the constant needs a literal load instead. */
static inline uint32_t psx_thumb_expand_imm(uint32_t value)
{
    if (value < 0x100u)
        return value;

    /* value == ROR(unrotated8, rot) with the top bit of unrotated8 set */
    for (uint32_t rot = 8; rot < 32; rot++)
    {
        const uint32_t unrotated = (value << rot) | (value >> (32u - rot));

        if ((unrotated & 0xffffff80u) == 0x00000080u)
            return (rot << 7) | (unrotated & 0x7fu);
    }

    /* the replicated byte patterns */
    const uint32_t b = value & 0xffu;

    if (value == ((b << 24) | (b << 16) | (b << 8) | b))
        return 0x300u | b;

    if (value == ((b << 24) | (b << 8)))
        return 0x200u | b;

    if (value == ((b << 16) | b))
        return 0x100u | b;

    return 0xffffffffu;
}

/* AND Rd, Rn, #modified_imm12 */
static inline void psx_emit_and_imm12(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf000u | (i << 10) | rn, (imm3 << 12) | (rd << 8) | imm8);
}

/* BIC Rd, Rn, #modified_imm12 */
static inline void psx_emit_bic_imm12(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf020u | (i << 10) | rn, (imm3 << 12) | (rd << 8) | imm8);
}

/* ANDS Rd, Rn, #modified_imm12 (sets the flags) */
static inline void psx_emit_ands_imm12(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf010u | (i << 10) | rn, (imm3 << 12) | (rd << 8) | imm8);
}

/* TST Rn, #modified_imm12 (ANDS with the result discarded) */
static inline void psx_emit_tst_imm12(psx_emit_t *e, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf010u | (i << 10) | rn, (imm3 << 12) | (0xfu << 8) | imm8);
}

/* ORR Rd, Rn, #modified_imm12 */
static inline void psx_emit_orr_imm12(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf040u | (i << 10) | rn, (imm3 << 12) | (rd << 8) | imm8);
}

/* EOR Rd, Rn, #modified_imm12 */
static inline void psx_emit_eor_imm12(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf080u | (i << 10) | rn, (imm3 << 12) | (rd << 8) | imm8);
}

/* CMP Rn, #modified_imm12 (32 bit form, any encodable constant) */
static inline void psx_emit_cmp_imm12(psx_emit_t *e, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf1b0u | (i << 10) | rn, (imm3 << 12) | (0xfu << 8) | imm8);
}

/* STRD Rt, Rt2, [Rn, #imm] (imm is a multiple of four, 0..1020) */
static inline void psx_emit_strd_imm(psx_emit_t *e, uint32_t rt, uint32_t rt2, uint32_t rn, uint32_t imm)
{
    psx_emit32(e, 0xe9c0u | rn, (rt << 12) | (rt2 << 8) | ((imm >> 2) & 0xffu));
}

/* SUB Rd, Rn, #imm12 (T4, no flags) */
static inline void psx_emit_sub_imm12(psx_emit_t *e, uint32_t rd, uint32_t rn, uint32_t imm12)
{
    const uint32_t i = (imm12 >> 11) & 1u;
    const uint32_t imm3 = (imm12 >> 8) & 0x7u;
    const uint32_t imm8 = imm12 & 0xffu;

    psx_emit32(e, 0xf2a0u | (i << 10) | rn, (imm3 << 12) | (rd << 8) | imm8);
}

/* ---------------------------------------------------------------- register offset memory */

/* LDR/LDRB/LDRH/LDRSB/LDRSH Rt, [Rn, Rm] */
static inline void psx_emit_ldr_reg(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xf850u | rn, (rt << 12) | rm);
}

static inline void psx_emit_ldrb_reg(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xf810u | rn, (rt << 12) | rm);
}

static inline void psx_emit_ldrh_reg(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xf830u | rn, (rt << 12) | rm);
}

static inline void psx_emit_ldrsb_reg(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xf910u | rn, (rt << 12) | rm);
}

static inline void psx_emit_ldrsh_reg(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xf930u | rn, (rt << 12) | rm);
}

/* STR/STRB/STRH Rt, [Rn, Rm] */
static inline void psx_emit_str_reg(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xf840u | rn, (rt << 12) | rm);
}

static inline void psx_emit_strb_reg(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xf800u | rn, (rt << 12) | rm);
}

static inline void psx_emit_strh_reg(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t rm)
{
    psx_emit32(e, 0xf820u | rn, (rt << 12) | rm);
}

/* LDRB Rt, [Rn, #imm12] (used for the code page table) */
static inline void psx_emit_ldrb_imm(psx_emit_t *e, uint32_t rt, uint32_t rn, uint32_t imm12)
{
    psx_emit32(e, 0xf890u | rn, (rt << 12) | (imm12 & 0xfffu));
}

/* ---------------------------------------------------------------- branches */

/* B.W <target> (unconditional, +-16MB) */
static inline void psx_emit_b(psx_emit_t *e, uint32_t target)
{
    const int32_t off = (int32_t)(target - (psx_emit_here(e) + 4u));
    const uint32_t s = ((uint32_t)off >> 24) & 1u;
    const uint32_t i1 = ((uint32_t)off >> 23) & 1u;
    const uint32_t i2 = ((uint32_t)off >> 22) & 1u;
    const uint32_t j1 = (~i1 ^ s) & 1u;
    const uint32_t j2 = (~i2 ^ s) & 1u;
    const uint32_t imm10 = ((uint32_t)off >> 12) & 0x3ffu;
    const uint32_t imm11 = ((uint32_t)off >> 1) & 0x7ffu;

    psx_emit32(e, 0xf000u | (s << 10) | imm10, 0x9000u | (j1 << 13) | (j2 << 11) | imm11);
}

/* B<cond>.W <target> (+-1MB) */
static inline void psx_emit_bcond(psx_emit_t *e, uint32_t cond, uint32_t target)
{
    const int32_t off = (int32_t)(target - (psx_emit_here(e) + 4u));
    const uint32_t s = ((uint32_t)off >> 20) & 1u;
    const uint32_t j2 = ((uint32_t)off >> 19) & 1u;
    const uint32_t j1 = ((uint32_t)off >> 18) & 1u;
    const uint32_t imm6 = ((uint32_t)off >> 12) & 0x3fu;
    const uint32_t imm11 = ((uint32_t)off >> 1) & 0x7ffu;

    psx_emit32(e, 0xf000u | (s << 10) | (cond << 6) | imm6, 0x8000u | (j1 << 13) | (j2 << 11) | imm11);
}

/* placeholder for a forward unconditional B.W, patched by psx_emit_patch_b */
static inline uint16_t *psx_emit_b_fwd(psx_emit_t *e)
{
    uint16_t *slot = e->cur;

    psx_emit32(e, 0xf000u, 0x9000u);

    return slot;
}

static inline void psx_emit_patch_b(uint16_t *slot, uint32_t target)
{
    const int32_t off = (int32_t)(target - ((uint32_t)(uintptr_t)slot + 4u));
    const uint32_t s = ((uint32_t)off >> 24) & 1u;
    const uint32_t i1 = ((uint32_t)off >> 23) & 1u;
    const uint32_t i2 = ((uint32_t)off >> 22) & 1u;
    const uint32_t j1 = (~i1 ^ s) & 1u;
    const uint32_t j2 = (~i2 ^ s) & 1u;
    const uint32_t imm10 = ((uint32_t)off >> 12) & 0x3ffu;
    const uint32_t imm11 = ((uint32_t)off >> 1) & 0x7ffu;

    slot[0] = (uint16_t)(0xf000u | (s << 10) | imm10);
    slot[1] = (uint16_t)(0x9000u | (j1 << 13) | (j2 << 11) | imm11);
}

/* placeholder for a forward B<cond>.W, patched later by psx_emit_patch_bcond */
static inline uint16_t *psx_emit_bcond_fwd(psx_emit_t *e, uint32_t cond)
{
    uint16_t *slot = e->cur;

    psx_emit32(e, 0xf000u | (cond << 6), 0x8000u);

    return slot;
}

static inline void psx_emit_patch_bcond(uint16_t *slot, uint32_t cond, uint32_t target)
{
    const int32_t off = (int32_t)(target - ((uint32_t)(uintptr_t)slot + 4u));
    const uint32_t s = ((uint32_t)off >> 20) & 1u;
    const uint32_t j2 = ((uint32_t)off >> 19) & 1u;
    const uint32_t j1 = ((uint32_t)off >> 18) & 1u;
    const uint32_t imm6 = ((uint32_t)off >> 12) & 0x3fu;
    const uint32_t imm11 = ((uint32_t)off >> 1) & 0x7ffu;

    slot[0] = (uint16_t)(0xf000u | (s << 10) | (cond << 6) | imm6);
    slot[1] = (uint16_t)(0x8000u | (j1 << 13) | (j2 << 11) | imm11);
}

/* BL <target> (call into C, +-16MB) */
static inline void psx_emit_bl(psx_emit_t *e, uint32_t target)
{
    const int32_t off = (int32_t)((target & ~1u) - (psx_emit_here(e) + 4u));
    const uint32_t s = ((uint32_t)off >> 24) & 1u;
    const uint32_t i1 = ((uint32_t)off >> 23) & 1u;
    const uint32_t i2 = ((uint32_t)off >> 22) & 1u;
    const uint32_t j1 = (~i1 ^ s) & 1u;
    const uint32_t j2 = (~i2 ^ s) & 1u;
    const uint32_t imm10 = ((uint32_t)off >> 12) & 0x3ffu;
    const uint32_t imm11 = ((uint32_t)off >> 1) & 0x7ffu;

    psx_emit32(e, 0xf000u | (s << 10) | imm10, 0xd000u | (j1 << 13) | (j2 << 11) | imm11);
}

/* BLX Rm */
static inline void psx_emit_blx(psx_emit_t *e, uint32_t rm)
{
    psx_emit16(e, 0x4780u | (rm << 3));
}

/* BX Rm */
static inline void psx_emit_bx(psx_emit_t *e, uint32_t rm)
{
    psx_emit16(e, 0x4700u | (rm << 3));
}

#ifdef __cplusplus
}
#endif

#endif
