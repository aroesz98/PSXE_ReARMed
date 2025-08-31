// fixed_math.h
// Fixed-point arithmetic for ARM Cortex-M7 (inline assembly, no external libs)
// Author: Arkadiusz (2025)

#ifndef FIXED_MATH_H
#define FIXED_MATH_H

#include <stdint.h>

// ------------------------------------
// Własne typy 128-bit
// ------------------------------------
typedef struct
{
    uint64_t lo;
    uint64_t hi;
} uint128_t;

typedef struct
{
    int64_t lo;
    int64_t hi;
} int128_t;

// ------------------------------------
// Stałe precyzji (bity ułamkowe)
// ------------------------------------
#define FIX4_FRAC_BITS 4   // int8_t / uint8_t
#define FIX8_FRAC_BITS 8   // int16_t / uint16_t
#define FIX16_FRAC_BITS 16 // int32_t / uint32_t
#define FIX32_FRAC_BITS 32 // int64_t / uint64_t / 128-bit

// ------------------------------------
// Makra pomocnicze
// ------------------------------------
#define TO_FIXED(x, frac) ((int64_t)((x) * (1LL << (frac))))
#define FROM_FIXED(x, frac) ((float)(x) / (float)(1LL << (frac)))

// -------------------------------------------------------------------
// 8-bit (4.4) — int8_t / uint8_t
// -------------------------------------------------------------------
static inline int8_t fixed4_add_i8(int8_t a, int8_t b) { return a + b; }
static inline int8_t fixed4_sub_i8(int8_t a, int8_t b) { return a - b; }
static inline uint8_t fixed4_add_u8(uint8_t a, uint8_t b) { return a + b; }
static inline uint8_t fixed4_sub_u8(uint8_t a, uint8_t b) { return a - b; }

static inline int8_t fixed4_mul_i8(int8_t a, int8_t b)
{
    int16_t r;
    __asm__ volatile(
        "smulbb %0, %1, %2\n"
        "asr    %0, %0, #4\n"
        : "=&r"(r)
        : "r"(a), "r"(b));
    return (int8_t)r;
}

static inline uint8_t fixed4_mul_u8(uint8_t a, uint8_t b)
{
    uint16_t r;
    __asm__ volatile(
        "mul %0, %1, %2\n"
        "lsr %0, %0, #4\n"
        : "=&r"(r)
        : "r"(a), "r"(b));
    return (uint8_t)r;
}

static inline int8_t fixed4_div_i8(int8_t a, int8_t b)
{
    int16_t r;
    __asm__ volatile(
        "lsl r4, %1, #4\n"
        "sdiv %0, r4, %2\n"
        : "=&r"(r)
        : "r"(a), "r"(b)
        : "r4");
    return (int8_t)r;
}

static inline uint8_t fixed4_div_u8(uint8_t a, uint8_t b)
{
    uint16_t r;
    __asm__ volatile(
        "lsl r4, %1, #4\n"
        "udiv %0, r4, %2\n"
        : "=&r"(r)
        : "r"(a), "r"(b)
        : "r4");
    return (uint8_t)r;
}

// -------------------------------------------------------------------
// 16-bit (8.8) — int16_t / uint16_t
// -------------------------------------------------------------------
static inline int16_t fixed8_add_i16(int16_t a, int16_t b) { return a + b; }
static inline int16_t fixed8_sub_i16(int16_t a, int16_t b) { return a - b; }
static inline uint16_t fixed8_add_u16(uint16_t a, uint16_t b) { return a + b; }
static inline uint16_t fixed8_sub_u16(uint16_t a, uint16_t b) { return a - b; }

static inline int16_t fixed8_mul_i16(int16_t a, int16_t b)
{
    int32_t r;
    __asm__ volatile(
        "smulbb %0, %1, %2\n"
        "asr    %0, %0, #8\n"
        : "=&r"(r)
        : "r"(a), "r"(b));
    return (int16_t)r;
}

static inline uint16_t fixed8_mul_u16(uint16_t a, uint16_t b)
{
    uint32_t r;
    __asm__ volatile(
        "mul %0, %1, %2\n"
        "lsr %0, %0, #8\n"
        : "=&r"(r)
        : "r"(a), "r"(b));
    return (uint16_t)r;
}

static inline int16_t fixed8_div_i16(int16_t a, int16_t b)
{
    int32_t r;
    __asm__ volatile(
        "lsl r4, %1, #8\n"
        "sdiv %0, r4, %2\n"
        : "=&r"(r)
        : "r"(a), "r"(b)
        : "r4");
    return (int16_t)r;
}

static inline uint16_t fixed8_div_u16(uint16_t a, uint16_t b)
{
    uint32_t r;
    __asm__ volatile(
        "lsl r4, %1, #8\n"
        "udiv %0, r4, %2\n"
        : "=&r"(r)
        : "r"(a), "r"(b)
        : "r4");
    return (uint16_t)r;
}

// -------------------------------------------------------------------
// 32-bit (16.16) — int32_t / uint32_t
// -------------------------------------------------------------------
static inline int32_t fixed16_add_i32(int32_t a, int32_t b) { return a + b; }
static inline int32_t fixed16_sub_i32(int32_t a, int32_t b) { return a - b; }
static inline uint32_t fixed16_add_u32(uint32_t a, uint32_t b) { return a + b; }
static inline uint32_t fixed16_sub_u32(uint32_t a, uint32_t b) { return a - b; }

static inline int32_t fixed16_mul_i32(int32_t a, int32_t b)
{
    int32_t r;
    __asm__ volatile(
        "smull %0, r4, %1, %2\n"
        "mov   %0, %0, lsr #16\n"
        : "=&r"(r)
        : "r"(a), "r"(b)
        : "r4");
    return r;
}

static inline uint32_t fixed16_mul_u32(uint32_t a, uint32_t b)
{
    uint32_t r;
    __asm__ volatile(
        "umull %0, r4, %1, %2\n"
        "lsr   %0, %0, #16\n"
        : "=&r"(r)
        : "r"(a), "r"(b)
        : "r4");
    return r;
}

static inline int32_t fixed16_div_i32(int32_t a, int32_t b)
{
    int32_t r;
    __asm__ volatile(
        "lsl   r4, %1, #16\n" // przesunięcie licznika w lewo o bity ułamkowe
        "sdiv  %0, r4, %2\n"  // dzielenie całkowite ze znakiem
        : "=&r"(r)
        : "r"(a), "r"(b)
        : "r4");
    return r;
}

static inline uint32_t fixed16_div_u32(uint32_t a, uint32_t b)
{
    uint32_t r;
    __asm__ volatile(
        "lsl   r4, %1, #16\n"
        "udiv  %0, r4, %2\n"
        : "=&r"(r)
        : "r"(a), "r"(b)
        : "r4");
    return r;
}

static inline int64_t fixed32_add_i64(int64_t a, int64_t b) { return a + b; }
static inline int64_t fixed32_sub_i64(int64_t a, int64_t b) { return a - b; }
static inline uint64_t fixed32_add_u64(uint64_t a, uint64_t b) { return a + b; }
static inline uint64_t fixed32_sub_u64(uint64_t a, uint64_t b) { return a - b; }

static inline int64_t fixed32_mul_i64(int64_t a, int64_t b)
{
    // 64×64 => 128, potem przesunięcie
    uint64_t a_lo = (uint32_t)a;
    uint64_t a_hi = (uint64_t)((uint64_t)a >> 32);
    uint64_t b_lo = (uint32_t)b;
    uint64_t b_hi = (uint64_t)((uint64_t)b >> 32);

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_hi * b_lo;
    uint64_t p2 = a_lo * b_hi;
    uint64_t p3 = a_hi * b_hi;

    uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    uint64_t lo = (middle << 32) | (uint32_t)p0;

    // 128-bit wynik w hi:lo, przesunięcie o 32 bits
    uint64_t shifted_lo = (lo >> 32) | (hi << (64 - 32));
    return (int64_t)shifted_lo;
}

static inline uint64_t fixed32_mul_u64(uint64_t a, uint64_t b)
{
    uint64_t a_lo = (uint32_t)a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint32_t)b;
    uint64_t b_hi = b >> 32;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_hi * b_lo;
    uint64_t p2 = a_lo * b_hi;
    uint64_t p3 = a_hi * b_hi;

    uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    uint64_t lo = (middle << 32) | (uint32_t)p0;

    uint64_t shifted_lo = (lo >> 32) | (hi << (64 - 32));
    return shifted_lo;
}

static inline int64_t fixed32_div_i64(int64_t a, int64_t b)
{
    return (a << 32) / b;
}

static inline uint64_t fixed32_div_u64(uint64_t a, uint64_t b)
{
    return (a << 32) / b;
}

static inline uint128_t u128_add(uint128_t a, uint128_t b)
{
    uint128_t r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}

static inline uint128_t u128_sub(uint128_t a, uint128_t b)
{
    uint128_t r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

static inline int128_t i128_add(int128_t a, int128_t b)
{
    int128_t r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + ((uint64_t)r.lo < (uint64_t)a.lo);
    return r;
}

static inline int128_t i128_sub(int128_t a, int128_t b)
{
    int128_t r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - ((uint64_t)a.lo < (uint64_t)b.lo);
    return r;
}

// proste mnożenie 64×64 => 128, przesunięcie do fixed32
static inline uint128_t fixed32_mul_u128(uint64_t a, uint64_t b)
{
    uint128_t r;
    uint64_t a_lo = (uint32_t)a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint32_t)b;
    uint64_t b_hi = b >> 32;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_hi * b_lo;
    uint64_t p2 = a_lo * b_hi;
    uint64_t p3 = a_hi * b_hi;

    uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    r.hi = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    r.lo = (middle << 32) | (uint32_t)p0;

    // przesunięcie 32-bit
    uint64_t new_lo = (r.lo >> 32) | (r.hi << (64 - 32));
    r.hi >>= 32;
    r.lo = new_lo;
    return r;
}

#endif // FIXED_MATH_H
