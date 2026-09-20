#ifndef PSX_JIT_H
#define PSX_JIT_H

/*
    Block recompiler for the PSX MIPS R3000A core.

    The recompiler and the interpreter are not two modes: every block is a
    sequence of translated instructions where anything the translator cannot
    (yet) emit natively falls back to the interpreter for that single
    instruction. A block is therefore never abandoned and behaviour stays
    identical to the pure interpreter.

    Written in C, kept C++ friendly (extern "C", POD types, explicit casts).
*/

#include <stdint.h>

#include "../cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set to 0 to run the plain interpreter (the JIT is bypassed everywhere) */
#ifndef PSX_JIT_ENABLE
#define PSX_JIT_ENABLE 1
#endif

/* Code cache lives in ITCM: zero wait state fetch and no cache maintenance */
#ifndef PSX_JIT_CODE_SIZE
#define PSX_JIT_CODE_SIZE (80 * 1024)
#endif

#ifndef PSX_JIT_MAX_BLOCKS
#define PSX_JIT_MAX_BLOCKS 1024
#endif

#ifndef PSX_JIT_HASH_SIZE
#define PSX_JIT_HASH_SIZE 2048 /* must be a power of two */
#endif

/* Instructions per block. Bounded so the device update granularity (and the
   interrupt latency) stays in the same range as the interpreter's. */
#ifndef PSX_JIT_MAX_INSTR
#define PSX_JIT_MAX_INSTR 16
#endif

typedef struct psx_jit_block_t
{
    uint32_t pc;                        /* guest address of the first instruction */
    uint32_t code;                      /* host entry point (Thumb, bit0 set)     */
    uint16_t instr;                     /* translated instructions                */
    uint16_t page;                      /* guest page this block was built from   */
    struct psx_jit_block_t *next;       /* hash chain                             */
    struct psx_jit_block_t *page_next;  /* chain of blocks built from one page    */
} psx_jit_block_t;

typedef struct
{
    uint32_t blocks;         /* blocks currently compiled        */
    uint32_t killed;         /* blocks dropped by invalidation   */
    uint32_t compiles;       /* blocks compiled since reset      */
    uint32_t flushes;        /* code cache flushes               */
    uint32_t invalidations;  /* invalidations from stores / DMA  */
    uint32_t code_used;      /* bytes of code cache in use       */
    uint32_t interp_steps;   /* instructions run by the fallback */
    uint32_t dispatch_steps; /* instructions the dispatcher had to run itself */
    uint32_t native;         /* instructions translated natively */
} psx_jit_stats_t;

/* Counts the instructions the fallback runs, per opcode class, so the next
   translation work can be aimed with numbers instead of guesses. Diagnostic
   only: set to 0 for the shipped firmware. */
#ifndef PSX_JIT_HIST
#define PSX_JIT_HIST 0
#endif

void psx_jit_init(void);
void psx_jit_reset(void);

/* Runs one block starting at cpu->pc and returns the emulated cycles it used */
uint32_t psx_jit_step(psx_cpu_t *cpu);

/* Guest memory changed (store, DMA, CD): drop any code compiled from it */
void psx_jit_invalidate(uint32_t addr, uint32_t size);

const psx_jit_stats_t *psx_jit_get_stats(void);

#if PSX_JIT_HIST
/* 0..63: primary opcode, 64..127: SPECIAL function code */
#define PSX_JIT_HIST_SIZE 128
const uint32_t *psx_jit_get_hist(void);
void psx_jit_clear_hist(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
