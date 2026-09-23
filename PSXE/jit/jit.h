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

/* Hot tier, in ITCM: zero wait state fetch and no cache maintenance. Holds
   copies of the blocks that run the most. */
#ifndef PSX_JIT_CODE_SIZE
#define PSX_JIT_CODE_SIZE (88 * 1024)
#endif

/* Every translated block, in SDRAM. Sized so that a scene's whole working set
   fits and nothing has to be translated twice; emptied as a whole when full. */
#ifndef PSX_JIT_MASTER_SIZE
#define PSX_JIT_MASTER_SIZE (4 * 1024 * 1024)
#endif

/* Blocks and links to not yet translated ones. Referred to by 16 bit index.
   Sized by measurement: FF7's battle module alone runs about 5300 different
   blocks within its first seconds, on top of the kernel's - with 4096 entries
   the pool overflowed, and everything was translated again, every second or
   two for as long as a fight lasted. */
#ifndef PSX_JIT_MAX_BLOCKS
#define PSX_JIT_MAX_BLOCKS 8192
#endif

#ifndef PSX_JIT_HASH_SIZE
#define PSX_JIT_HASH_SIZE 8192 /* must be a power of two */
#endif

/* Instructions per block. Bounded so the device update granularity (and the
   interrupt latency) stays in the same range as the interpreter's. */
#ifndef PSX_JIT_MAX_INSTR
#define PSX_JIT_MAX_INSTR 16
#endif

typedef struct
{
    uint32_t blocks;         /* blocks and links in use          */
    uint32_t killed;         /* blocks dropped by invalidation   */
    uint32_t compiles;       /* blocks compiled since reset      */
    uint32_t flushes;        /* times the code area was emptied  */
    uint32_t invalidations;  /* invalidations from stores / DMA  */
    uint32_t code_used;      /* bytes of the SDRAM code area in use */
    uint32_t interp_steps;   /* instructions run by the fallback */
    uint32_t dispatch_steps; /* instructions the dispatcher had to run itself */
    uint32_t native;         /* instructions translated natively */
    uint32_t hot_used;       /* bytes of the ITCM tier in use    */
    uint32_t hot_blocks;     /* blocks running from ITCM         */
    uint32_t retiers;        /* times the ITCM tier was revised  */
    uint32_t dispatches;     /* blocks entered from the dispatcher */
    uint32_t cold_dispatches;/* of those, into a block running from SDRAM */
    uint32_t slow_mem;       /* loads / stores through the slow path helper */
} psx_jit_stats_t;

/* Counts the instructions the fallback runs, per opcode class, so the next
   translation work can be aimed with numbers instead of guesses. Diagnostic
   only: set to 0 for the shipped firmware. */
#ifndef PSX_JIT_HIST
#define PSX_JIT_HIST 0
#endif

void psx_jit_init(void);
void psx_jit_reset(void);

/* Runs translated code starting at cpu->pc and returns the emulated cycles it
   used. Blocks run one another directly for as long as fewer than `budget`
   cycles have been used, so that is roughly how long this stays away. */
uint32_t psx_jit_step(psx_cpu_t *cpu, uint32_t budget);

/* Guest memory changed (store, DMA, CD): drop any code compiled from it */
void psx_jit_invalidate(uint32_t addr, uint32_t size);

const psx_jit_stats_t *psx_jit_get_stats(void);

/* Where translated code lives: out[0..1] the fast tier, out[2..3] the master
   area, as first and one past the last address. For the PC sampler. */
void psx_jit_code_ranges(uint32_t out[4]);

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
