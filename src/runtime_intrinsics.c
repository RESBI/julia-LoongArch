// This file is a part of Julia. License is MIT: https://julialang.org/license

// This is in implementation of the Julia intrinsic functions against boxed types
// excluding the native function call interface (ccall, llvmcall)
//
// this file assumes a little-endian processor, although that isn't too hard to fix
// it also assumes two's complement negative numbers, which might be a bit harder to fix

#include "APInt-C.h"
#include "julia.h"
#include "julia_internal.h"
#include "llvm-version.h"

const unsigned int host_char_bit = 8;

// float16 conversion helpers

static inline float half_to_float(uint16_t ival) JL_NOTSAFEPOINT
{
    uint32_t sign = (ival & 0x8000) >> 15;
    uint32_t exp = (ival & 0x7c00) >> 10;
    uint32_t sig = (ival & 0x3ff) >> 0;
    uint32_t ret;

    if (exp == 0) {
        if (sig == 0) {
            sign = sign << 31;
            ret = sign | exp | sig;
        }
        else {
            int n_bit = 1;
            uint16_t bit = 0x0200;
            while ((bit & sig) == 0) {
                n_bit = n_bit + 1;
                bit = bit >> 1;
            }
            sign = sign << 31;
            exp = ((-14 - n_bit + 127) << 23);
            sig = ((sig & (~bit)) << n_bit) << (23 - 10);
            ret = sign | exp | sig;
        }
    }
    else if (exp == 0x1f) {
        if (sig == 0) { // Inf
            if (sign == 0)
                ret = 0x7f800000;
            else
                ret = 0xff800000;
        }
        else // NaN
            ret = 0x7fc00000 | (sign << 31) | (sig << (23 - 10));
    }
    else {
        sign = sign << 31;
        exp = ((exp - 15 + 127) << 23);
        sig = sig << (23 - 10);
        ret = sign | exp | sig;
    }

    float fret;
    memcpy(&fret, &ret, sizeof(float));
    return fret;
}

// float to half algorithm from:
//   "Fast Half Float Conversion" by Jeroen van der Zijp
//   ftp://ftp.fox-toolkit.org/pub/fasthalffloatconversion.pdf
//
// With adjustments for round-to-nearest, ties to even.

static uint16_t basetable[512] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0400, 0x0800, 0x0c00, 0x1000, 0x1400, 0x1800, 0x1c00, 0x2000,
    0x2400, 0x2800, 0x2c00, 0x3000, 0x3400, 0x3800, 0x3c00, 0x4000, 0x4400, 0x4800, 0x4c00,
    0x5000, 0x5400, 0x5800, 0x5c00, 0x6000, 0x6400, 0x6800, 0x6c00, 0x7000, 0x7400, 0x7800,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00,
    0x7c00, 0x7c00, 0x7c00, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8400, 0x8800, 0x8c00, 0x9000, 0x9400,
    0x9800, 0x9c00, 0xa000, 0xa400, 0xa800, 0xac00, 0xb000, 0xb400, 0xb800, 0xbc00, 0xc000,
    0xc400, 0xc800, 0xcc00, 0xd000, 0xd400, 0xd800, 0xdc00, 0xe000, 0xe400, 0xe800, 0xec00,
    0xf000, 0xf400, 0xf800, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00,
    0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00, 0xfc00};

static uint8_t shifttable[512] = {
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0f,
    0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d,
    0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d,
    0x0d, 0x0d, 0x0d, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x0d, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13,
    0x12, 0x11, 0x10, 0x0f, 0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d,
    0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d,
    0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x0d};

static inline uint16_t float_to_half(float param) JL_NOTSAFEPOINT
{
    uint32_t f;
    memcpy(&f, &param, sizeof(float));
    if (isnan(param)) {
        // Match the behaviour of arm64's fcvt or x86's vcvtps2ph by quieting
        // all NaNs (avoids creating infinities), preserving the sign, and using
        // the upper bits of the payload.
        //      sign              exp      quiet    payload
        return (f>>16 & 0x8000) | 0x7c00 | 0x0200 | (f>>13 & 0x03ff);
    }
    int i = ((f & ~0x007fffff) >> 23);
    uint8_t sh = shifttable[i];
    f &= 0x007fffff;
    // If `val` is subnormal, the tables are set up to force the
    // result to 0, so the significand has an implicit `1` in the
    // cases we care about.
    f |= 0x007fffff + 0x1;
    uint16_t h = (uint16_t)(basetable[i] + ((f >> sh) & 0x03ff));
    // round
    // NOTE: we maybe should ignore NaNs here, but the payload is
    // getting truncated anyway so "rounding" it might not matter
    int nextbit = (f >> (sh - 1)) & 1;
    if (nextbit != 0 && (h & 0x7C00) != 0x7C00) {
        // Round halfway to even or check lower bits
        if ((h & 1) == 1 || (f & ((1 << (sh - 1)) - 1)) != 0)
            h += UINT16_C(1);
    }
    return h;
}

static inline uint16_t double_to_half(double param) JL_NOTSAFEPOINT
{
    float temp = (float)param;
    uint32_t tempi;
    memcpy(&tempi, &temp, sizeof(temp));

    // if Float16(res) is subnormal
    if ((tempi&0x7fffffffu) < 0x38800000u) {
        // shift so that the mantissa lines up where it would for normal Float16
        uint32_t shift = 113u-((tempi & 0x7f800000u)>>23u);
        if (shift<23u) {
            tempi |= 0x00800000; // set implicit bit
            tempi >>= shift;
        }
    }

    // if we are halfway between 2 Float16 values
    if ((tempi & 0x1fffu) == 0x1000u) {
        memcpy(&tempi, &temp, sizeof(temp));
        // adjust the value by 1 ULP in the direction that will make Float16(temp) give the right answer
        tempi += (fabs(temp) < fabs(param)) - (fabs(param) < fabs(temp));
        memcpy(&temp, &tempi, sizeof(temp));
    }

    return float_to_half(temp);
}

// x86-specific helpers for emulating the (B)Float16 ABI
#if defined(_CPU_X86_) || defined(_CPU_X86_64_)
#include <xmmintrin.h>
__attribute__((unused)) static inline __m128 return_in_xmm(uint16_t input) JL_NOTSAFEPOINT {
    __m128 xmm_output;
    asm (
        "movd %[input], %%xmm0\n\t"
        "movss %%xmm0, %[xmm_output]\n\t"
        : [xmm_output] "=x" (xmm_output)
        : [input] "r" ((uint32_t)input)
        : "xmm0"
    );
    return xmm_output;
}
__attribute__((unused)) static inline uint16_t take_from_xmm(__m128 xmm_input) JL_NOTSAFEPOINT {
    uint32_t output;
    asm (
        "movss %[xmm_input], %%xmm0\n\t"
        "movd %%xmm0, %[output]\n\t"
        : [output] "=r" (output)
        : [xmm_input] "x" (xmm_input)
        : "xmm0"
    );
    return (uint16_t)output;
}
#endif

// float16 conversion API

// for use in APInt and other soft-float ABIs (i.e. without the ABI shenanigans from below)
JL_DLLEXPORT uint16_t julia_float_to_half(float param) {
    return float_to_half(param);
}
JL_DLLEXPORT uint16_t julia_double_to_half(double param) {
    return double_to_half(param);
}
JL_DLLEXPORT float julia_half_to_float(uint16_t param) {
    return half_to_float(param);
}

// starting with GCC 12 and Clang 15, we have _Float16 on most platforms
// (but not on Windows; this may be a bug in the MSYS2 GCC compilers)
#if ((defined(__GNUC__) && __GNUC__ > 11) || \
     (defined(__clang__) && __clang_major__ > 14)) && \
    !defined(_CPU_PPC64_) && !defined(_CPU_PPC_) && \
    !defined(_OS_WINDOWS_) && !defined(_CPU_RISCV64_) && !defined(_CPU_LOONGARCH64_)
    #define FLOAT16_TYPE _Float16
    #define FLOAT16_TO_UINT16(x) (*(uint16_t*)&(x))
    #define FLOAT16_FROM_UINT16(x) (*(_Float16*)&(x))
// on older compilers, we need to emulate the platform-specific ABI
#elif defined(_CPU_X86_) || (defined(_CPU_X86_64_) && !defined(_OS_WINDOWS_))
    // on x86, we can use __m128; except on Windows where x64 calling
    // conventions expect to pass __m128 by reference.
    #define FLOAT16_TYPE __m128
    #define FLOAT16_TO_UINT16(x) take_from_xmm(x)
    #define FLOAT16_FROM_UINT16(x) return_in_xmm(x)
#elif defined(_CPU_PPC64_) || defined(_CPU_PPC_)
    // on PPC, pass Float16 as if it were an integer, similar to the old x86 ABI
    // before _Float16
    #define FLOAT16_TYPE uint16_t
    #define FLOAT16_TO_UINT16(x) (x)
    #define FLOAT16_FROM_UINT16(x) (x)
#else
    // otherwise, pass using floating-point calling conventions
    #define FLOAT16_TYPE float
    #define FLOAT16_TO_UINT16(x) ((uint16_t)*(uint32_t*)&(x))
    #define FLOAT16_FROM_UINT16(x) ({ uint32_t tmp = (uint32_t)(x); *(float*)&tmp; })
#endif

JL_DLLEXPORT float julia__gnu_h2f_ieee(FLOAT16_TYPE param)
{
    uint16_t param16 = FLOAT16_TO_UINT16(param);
    return half_to_float(param16);
}

JL_DLLEXPORT FLOAT16_TYPE julia__gnu_f2h_ieee(float param)
{
    uint16_t res = float_to_half(param);
    return FLOAT16_FROM_UINT16(res);
}

JL_DLLEXPORT FLOAT16_TYPE julia__truncdfhf2(double param)
{
    uint16_t res = double_to_half(param);
    return FLOAT16_FROM_UINT16(res);
}


// bfloat16 conversion helpers

static inline uint16_t float_to_bfloat(float param) JL_NOTSAFEPOINT
{
    if (isnan(param))
        return 0x7fc0;

    uint32_t bits = *((uint32_t*) &param);

    // round to nearest even
    bits += 0x7fff + ((bits >> 16) & 1);
    return (uint16_t)(bits >> 16);
}

static inline uint16_t double_to_bfloat(double param) JL_NOTSAFEPOINT
{
    float temp = (float)param;
    uint32_t tempi;
    memcpy(&tempi, &temp, sizeof(temp));

    // bfloat16 uses the same exponent as float32, so we don't need special handling
    // for subnormals when truncating float64 to bfloat16.

    // if we are halfway between 2 bfloat16 values
    if ((tempi & 0x1ffu) == 0x100u) {
        // adjust the value by 1 ULP in the direction that will make bfloat16(temp) give the right answer
        tempi += (fabs(temp) < fabs(param)) - (fabs(param) < fabs(temp));
        memcpy(&temp, &tempi, sizeof(temp));
    }

    return float_to_bfloat(temp);
}

static inline float bfloat_to_float(uint16_t param) JL_NOTSAFEPOINT
{
    uint32_t bits = ((uint32_t)param) << 16;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

// bfloat16 conversion API

// for use in APInt (without the ABI shenanigans from below)
uint16_t julia_float_to_bfloat(float param) {
    return float_to_bfloat(param);
}
float julia_bfloat_to_float(uint16_t param) {
    return bfloat_to_float(param);
}

// starting with GCC 13 and Clang 17, we have __bf16 on most platforms
// (but not on Windows; this may be a bug in the MSYS2 GCC compilers)
#if ((defined(__GNUC__) && __GNUC__ > 12) || \
     (defined(__clang__) && __clang_major__ > 16)) && \
    !defined(_CPU_PPC64_) && !defined(_CPU_PPC_) && \
    !defined(_OS_WINDOWS_) && !defined(_CPU_RISCV64_) && !defined(_CPU_LOONGARCH64_)
    #define BFLOAT16_TYPE __bf16
    #define BFLOAT16_TO_UINT16(x) (*(uint16_t*)&(x))
    #define BFLOAT16_FROM_UINT16(x) (*(__bf16*)&(x))
// on older compilers, we need to emulate the platform-specific ABI.
// for more details, see similar code above that deals with Float16.
#elif defined(_CPU_X86_) || (defined(_CPU_X86_64_) && !defined(_OS_WINDOWS_))
    #define BFLOAT16_TYPE __m128
    #define BFLOAT16_TO_UINT16(x) take_from_xmm(x)
    #define BFLOAT16_FROM_UINT16(x) return_in_xmm(x)
#elif defined(_CPU_PPC64_) || defined(_CPU_PPC_)
    #define BFLOAT16_TYPE uint16_t
    #define BFLOAT16_TO_UINT16(x) (x)
    #define BFLOAT16_FROM_UINT16(x) (x)
#else
    #define BFLOAT16_TYPE float
    #define BFLOAT16_TO_UINT16(x) ((uint16_t)*(uint32_t*)&(x))
    #define BFLOAT16_FROM_UINT16(x) ({ uint32_t tmp = (uint32_t)(x); *(float*)&tmp; })
#endif

JL_DLLEXPORT BFLOAT16_TYPE julia__truncsfbf2(float param) JL_NOTSAFEPOINT
{
    uint16_t res = float_to_bfloat(param);
    return BFLOAT16_FROM_UINT16(res);
}

JL_DLLEXPORT BFLOAT16_TYPE julia__truncdfbf2(double param) JL_NOTSAFEPOINT
{
    uint16_t res = double_to_bfloat(param);
    return BFLOAT16_FROM_UINT16(res);
}


// run time version of bitcast intrinsic
JL_DLLEXPORT jl_value_t *jl_bitcast(jl_value_t *ty, jl_value_t *v)
{
    JL_TYPECHK(bitcast, datatype, ty);
    if (!jl_is_concrete_type(ty) || !jl_is_primitivetype(ty))
        jl_error("bitcast: target type not a leaf primitive type");
    if (!jl_is_primitivetype(jl_typeof(v)))
        jl_error("bitcast: value not a primitive type");
    if (jl_datatype_size(jl_typeof(v)) != jl_datatype_size(ty))
        jl_error("bitcast: argument size does not match size of target type");
    if (ty == jl_typeof(v))
        return v;
    if (ty == (jl_value_t*)jl_bool_type)
        return *(uint8_t*)jl_data_ptr(v) & 1 ? jl_true : jl_false;
    return jl_new_bits(ty, jl_data_ptr(v));
}

// run time version of pointerref intrinsic (warning: i is not rooted)
JL_DLLEXPORT jl_value_t *jl_pointerref(jl_value_t *p, jl_value_t *i, jl_value_t *align)
{
    JL_TYPECHK(pointerref, pointer, p);
    JL_TYPECHK(pointerref, long, i)
    JL_TYPECHK(pointerref, long, align);
    jl_value_t *ety = jl_tparam0(jl_typeof(p));
    if (ety == (jl_value_t*)jl_any_type) {
        jl_value_t **pp = (jl_value_t**)(jl_unbox_long(p) + (jl_unbox_long(i)-1)*sizeof(void*));
        return *pp;
    }
    else {
        if (!is_valid_intrinsic_elptr(ety))
            jl_error("pointerref: invalid pointer");
        size_t nb = LLT_ALIGN(jl_datatype_size(ety), jl_datatype_align(ety));
        char *pp = (char*)jl_unbox_long(p) + (jl_unbox_long(i)-1)*nb;
        return jl_new_bits(ety, pp);
    }
}

// run time version of pointerset intrinsic (warning: x is not gc-rooted)
JL_DLLEXPORT jl_value_t *jl_pointerset(jl_value_t *p, jl_value_t *x, jl_value_t *i, jl_value_t *align)
{
    JL_TYPECHK(pointerset, pointer, p);
    JL_TYPECHK(pointerset, long, i);
    JL_TYPECHK(pointerset, long, align);
    jl_value_t *ety = jl_tparam0(jl_typeof(p));
    if (ety == (jl_value_t*)jl_any_type) {
        jl_value_t **pp = (jl_value_t**)(jl_unbox_long(p) + (jl_unbox_long(i)-1)*sizeof(void*));
        *pp = x;
    }
    else {
        if (!is_valid_intrinsic_elptr(ety))
            jl_error("pointerset: invalid pointer");
        if (jl_typeof(x) != ety)
            jl_type_error("pointerset", ety, x);
        size_t elsz = jl_datatype_size(ety);
        size_t nb = LLT_ALIGN(elsz, jl_datatype_align(ety));
        char *pp = (char*)jl_unbox_long(p) + (jl_unbox_long(i)-1)*nb;
        memcpy(pp, x, elsz);
    }
    return p;
}

JL_DLLEXPORT jl_value_t *jl_atomic_pointerref(jl_value_t *p, jl_value_t *order)
{
    JL_TYPECHK(atomic_pointerref, pointer, p);
    JL_TYPECHK(atomic_pointerref, symbol, order)
    (void)jl_get_atomic_order_checked((jl_sym_t*)order, 1, 0);
    jl_value_t *ety = jl_tparam0(jl_typeof(p));
    char *pp = (char*)jl_unbox_long(p);
    if (ety == (jl_value_t*)jl_any_type) {
        return jl_atomic_load((_Atomic(jl_value_t*)*)pp);
    }
    else {
        if (!is_valid_intrinsic_elptr(ety))
            jl_error("atomic_pointerref: invalid pointer");
        size_t nb = jl_datatype_size(ety);
        if ((nb & (nb - 1)) != 0 || nb > MAX_POINTERATOMIC_SIZE)
            jl_error("atomic_pointerref: invalid pointer for atomic operation");
        return jl_atomic_new_bits(ety, pp);
    }
}

JL_DLLEXPORT jl_value_t *jl_atomic_pointerset(jl_value_t *p, jl_value_t *x, jl_value_t *order)
{
    JL_TYPECHK(atomic_pointerset, pointer, p);
    JL_TYPECHK(atomic_pointerset, symbol, order);
    (void)jl_get_atomic_order_checked((jl_sym_t*)order, 0, 1);
    jl_value_t *ety = jl_tparam0(jl_typeof(p));
    char *pp = (char*)jl_unbox_long(p);
    if (ety == (jl_value_t*)jl_any_type) {
        jl_atomic_store((_Atomic(jl_value_t*)*)pp, x);
    }
    else {
        if (!is_valid_intrinsic_elptr(ety))
            jl_error("atomic_pointerset: invalid pointer");
        if (jl_typeof(x) != ety)
            jl_type_error("atomic_pointerset", ety, x);
        size_t nb = jl_datatype_size(ety);
        if ((nb & (nb - 1)) != 0 || nb > MAX_POINTERATOMIC_SIZE)
            jl_error("atomic_pointerset: invalid pointer for atomic operation");
        jl_atomic_store_bits(pp, x, nb);
    }
    return p;
}

JL_DLLEXPORT jl_value_t *jl_atomic_pointerswap(jl_value_t *p, jl_value_t *x, jl_value_t *order)
{
    JL_TYPECHK(atomic_pointerswap, pointer, p);
    JL_TYPECHK(atomic_pointerswap, symbol, order);
    (void)jl_get_atomic_order_checked((jl_sym_t*)order, 1, 1);
    jl_value_t *ety = jl_tparam0(jl_typeof(p));
    jl_value_t *y;
    char *pp = (char*)jl_unbox_long(p);
    if (ety == (jl_value_t*)jl_any_type) {
        y = jl_atomic_exchange((_Atomic(jl_value_t*)*)pp, x);
    }
    else {
        if (!is_valid_intrinsic_elptr(ety))
            jl_error("atomic_pointerswap: invalid pointer");
        if (jl_typeof(x) != ety)
            jl_type_error("atomic_pointerswap", ety, x);
        size_t nb = jl_datatype_size(ety);
        if ((nb & (nb - 1)) != 0 || nb > MAX_POINTERATOMIC_SIZE)
            jl_error("atomic_pointerswap: invalid pointer for atomic operation");
        y = jl_atomic_swap_bits(ety, pp, x, nb);
    }
    return y;
}

JL_DLLEXPORT jl_value_t *jl_atomic_pointermodify(jl_value_t *p, jl_value_t *f, jl_value_t *x, jl_value_t *order)
{
    JL_TYPECHK(atomic_pointermodify, pointer, p);
    JL_TYPECHK(atomic_pointermodify, symbol, order)
    (void)jl_get_atomic_order_checked((jl_sym_t*)order, 1, 1);
    jl_value_t *ety = jl_tparam0(jl_typeof(p));
    char *pp = (char*)jl_unbox_long(p);
    jl_value_t *expected;
    if (ety == (jl_value_t*)jl_any_type) {
        expected = jl_atomic_load((_Atomic(jl_value_t*)*)pp);
    }
    else {
        if (!is_valid_intrinsic_elptr(ety))
            jl_error("atomic_pointermodify: invalid pointer");
        size_t nb = jl_datatype_size(ety);
        if ((nb & (nb - 1)) != 0 || nb > MAX_POINTERATOMIC_SIZE)
            jl_error("atomic_pointermodify: invalid pointer for atomic operation");
        expected = jl_atomic_new_bits(ety, pp);
    }
    jl_value_t **args;
    JL_GC_PUSHARGS(args, 2);
    args[0] = expected;
    while (1) {
        args[1] = x;
        jl_value_t *y = jl_apply_generic(f, args, 2);
        args[1] = y;
        if (ety == (jl_value_t*)jl_any_type) {
            if (jl_atomic_cmpswap((_Atomic(jl_value_t*)*)pp, &expected, y))
                break;
        }
        else {
            //if (!is_valid_intrinsic_elptr(ety)) // handled by jl_atomic_pointerref earlier
            //    jl_error("atomic_pointermodify: invalid pointer");
            if (jl_typeof(y) != ety)
                jl_type_error("atomic_pointermodify", ety, y);
            size_t nb = jl_datatype_size(ety);
            if (jl_atomic_bool_cmpswap_bits(pp, expected, y, nb))
                break;
            expected = jl_atomic_new_bits(ety, pp);
        }
        args[0] = expected;
        jl_gc_safepoint();
    }
    // args[0] == expected (old)
    // args[1] == y (new)
    jl_datatype_t *rettyp = jl_apply_modify_type(ety);
    JL_GC_PROMISE_ROOTED(rettyp); // (JL_ALWAYS_LEAFTYPE)
    args[0] = jl_new_struct(rettyp, args[0], args[1]);
    JL_GC_POP();
    return args[0];
}


JL_DLLEXPORT jl_value_t *jl_atomic_pointerreplace(jl_value_t *p, jl_value_t *expected, jl_value_t *x, jl_value_t *success_order_sym, jl_value_t *failure_order_sym)
{
    JL_TYPECHK(atomic_pointerreplace, pointer, p);
    JL_TYPECHK(atomic_pointerreplace, symbol, success_order_sym);
    JL_TYPECHK(atomic_pointerreplace, symbol, failure_order_sym);
    enum jl_memory_order success_order = jl_get_atomic_order_checked((jl_sym_t*)success_order_sym, 1, 1);
    enum jl_memory_order failure_order = jl_get_atomic_order_checked((jl_sym_t*)failure_order_sym, 1, 0);
    if (failure_order > success_order)
        jl_atomic_error("atomic_pointerreplace: invalid atomic ordering");
    // TODO: filter other invalid orderings
    jl_value_t *ety = jl_tparam0(jl_typeof(p));
    if (!is_valid_intrinsic_elptr(ety))
        jl_error("atomic_pointerreplace: invalid pointer");
    char *pp = (char*)jl_unbox_long(p);
    jl_datatype_t *rettyp = jl_apply_cmpswap_type(ety);
    JL_GC_PROMISE_ROOTED(rettyp); // (JL_ALWAYS_LEAFTYPE)
    jl_value_t *result = NULL;
    JL_GC_PUSH1(&result);
    if (ety == (jl_value_t*)jl_any_type) {
        result = expected;
        int success;
        while (1) {
            success = jl_atomic_cmpswap((_Atomic(jl_value_t*)*)pp, &result, x);
            if (success || !jl_egal(result, expected))
                break;
        }
        result = jl_new_struct(rettyp, result, success ? jl_true : jl_false);
    }
    else {
        if (jl_typeof(x) != ety)
            jl_type_error("atomic_pointerreplace", ety, x);
        size_t nb = jl_datatype_size(ety);
        if ((nb & (nb - 1)) != 0 || nb > MAX_POINTERATOMIC_SIZE)
            jl_error("atomic_pointerreplace: invalid pointer for atomic operation");
        int isptr = jl_field_isptr(rettyp, 0);
        jl_task_t *ct = jl_current_task;
        result = jl_gc_alloc(ct->ptls, isptr ? nb : jl_datatype_size(rettyp), isptr ? ety : (jl_value_t*)rettyp);
        int success = jl_atomic_cmpswap_bits((jl_datatype_t*)ety, result, pp, expected, x, nb);
        if (isptr) {
            jl_value_t *z = jl_gc_alloc(ct->ptls, jl_datatype_size(rettyp), rettyp);
            *(jl_value_t**)z = result;
            result = z;
            nb = sizeof(jl_value_t*);
        }
        *((uint8_t*)result + nb) = success ? 1 : 0;
    }
    JL_GC_POP();
    return result;
}

JL_DLLEXPORT jl_value_t *jl_atomic_fence(jl_value_t *order_sym)
{
    JL_TYPECHK(fence, symbol, order_sym);
    enum jl_memory_order order = jl_get_atomic_order_checked((jl_sym_t*)order_sym, 1, 1);
    if (order > jl_memory_order_monotonic)
        jl_fence();
    return jl_nothing;
}

JL_DLLEXPORT jl_value_t *jl_cglobal(jl_value_t *v, jl_value_t *ty)
{
    JL_TYPECHK(cglobal, type, ty);
    JL_GC_PUSH1(&v);
    jl_value_t *rt =
        ty == (jl_value_t*)jl_nothing_type ? (jl_value_t*)jl_voidpointer_type : // a common case
            (jl_value_t*)jl_apply_type1((jl_value_t*)jl_pointer_type, ty);
    JL_GC_PROMISE_ROOTED(rt); // (JL_ALWAYS_LEAFTYPE)

    if (!jl_is_concrete_type(rt))
        jl_error("cglobal: type argument not concrete");

    if (jl_is_tuple(v) && jl_nfields(v) == 1)
        v = jl_fieldref(v, 0);

    if (jl_is_pointer(v)) {
        v = jl_bitcast(rt, v);
        JL_GC_POP();
        return v;
    }

    char *f_lib = NULL;
    if (jl_is_tuple(v) && jl_nfields(v) > 1) {
        jl_value_t *t1 = jl_fieldref(v, 1);
        if (jl_is_symbol(t1))
            f_lib = jl_symbol_name((jl_sym_t*)t1);
        else if (jl_is_string(t1))
            f_lib = jl_string_data(t1);
        else
            JL_TYPECHK(cglobal, symbol, t1)
        v = jl_fieldref(v, 0);
    }

    char *f_name = NULL;
    if (jl_is_symbol(v))
        f_name = jl_symbol_name((jl_sym_t*)v);
    else if (jl_is_string(v))
        f_name = jl_string_data(v);
    else
        JL_TYPECHK(cglobal, symbol, v)

    if (!f_lib)
        f_lib = (char*)jl_dlfind(f_name);

    void *ptr;
    jl_dlsym(jl_get_library(f_lib), f_name, &ptr, 1);
    jl_value_t *jv = jl_gc_alloc(jl_current_task->ptls, sizeof(void*), rt);
    *(void**)jl_data_ptr(jv) = ptr;
    JL_GC_POP();
    return jv;
}

JL_DLLEXPORT jl_value_t *jl_cglobal_auto(jl_value_t *v) {
    return jl_cglobal(v, (jl_value_t*)jl_nothing_type);
}

static inline char signbitbyte(void *a, unsigned bytes) JL_NOTSAFEPOINT
{
    // sign bit of an signed number of n bytes, as a byte
    return (((signed char*)a)[bytes - 1] < 0) ? ~0 : 0;
}

static inline char usignbitbyte(void *a, unsigned bytes) JL_NOTSAFEPOINT
{
    // sign bit of an unsigned number
    return 0;
}

static inline unsigned select_by_size(unsigned sz) JL_NOTSAFEPOINT
{
    /* choose the right sized function specialization */
    switch (sz) {
    default: return 0;
    case  1: return 1;
    case  2: return 2;
    case  4: return 3;
    case  8: return 4;
    case 16: return 5;
    }
}

#define SELECTOR_FUNC(intrinsic) \
    typedef intrinsic##_t select_##intrinsic##_t[6]; \
    static inline intrinsic##_t select_##intrinsic(unsigned sz, const select_##intrinsic##_t list) JL_NOTSAFEPOINT \
    { \
        intrinsic##_t thunk = list[select_by_size(sz)]; \
        if (!thunk) thunk = list[0]; \
        return thunk; \
    }

#define fp_select(a, func) \
    sizeof(a) <= sizeof(float) ? func##f((float)a) : func(a)
#define fp_select2(a, b, func) \
    sizeof(a) <= sizeof(float) ? func##f(a, b) : func(a, b)

// fast-function generators //

// integer input
// OP::Function macro(input)
// name::unique string
// nbits::number of bits
// c_type::c_type corresponding to nbits
#define un_iintrinsic_ctype(OP, name, nbits, c_type) \
static inline void jl_##name##nbits(unsigned runtime_nbits, void *pa, void *pr) JL_NOTSAFEPOINT \
{ \
    c_type a = *(c_type*)pa; \
    *(c_type*)pr = OP(a); \
}

// integer input, unsigned output
// OP::Function macro(input)
// name::unique string
// nbits::number of bits
// c_type::c_type corresponding to nbits
#define uu_iintrinsic_ctype(OP, name, nbits, c_type) \
static inline unsigned jl_##name##nbits(unsigned runtime_nbits, void *pa) JL_NOTSAFEPOINT \
{ \
    c_type a = *(c_type*)pa; \
    return OP(a); \
}

// floating point
// OP::Function macro(output pointer, input)
// name::unique string
// nbits::number of bits in the *input*
// c_type::c_type corresponding to nbits
#define un_fintrinsic_ctype(OP, name, c_type) \
static inline void name(unsigned osize, jl_value_t *ty, void *pa, void *pr) JL_NOTSAFEPOINT \
{ \
    c_type a = *(c_type*)pa; \
    OP(ty, (c_type*)pr, a); \
}

#define un_fintrinsic_half(OP, name)                                            \
    static inline void name(unsigned osize, jl_value_t *ty, void *pa, void *pr) \
        JL_NOTSAFEPOINT                                                         \
    {                                                                           \
        uint16_t a = *(uint16_t *)pa;                                           \
        float R, A = half_to_float(a);                                          \
        OP(ty, &R, A);                                                          \
        *(uint16_t *)pr = float_to_half(R);                                     \
    }

#define un_fintrinsic_bfloat(OP, name)                                          \
    static inline void name(unsigned osize, jl_value_t *ty, void *pa, void *pr) \
        JL_NOTSAFEPOINT                                                         \
    {                                                                           \
        uint16_t a = *(uint16_t *)pa;                                           \
        float R, A = bfloat_to_float(a);                                        \
        OP(ty, &R, A);                                                          \
        *(uint16_t *)pr = float_to_bfloat(R);                                   \
    }

// float or integer inputs
// OP::Function macro(inputa, inputb)
// name::unique string
// nbits::number of bits
// c_type::c_type corresponding to nbits
#define bi_intrinsic_ctype(OP, name, nbits, c_type) \
static void jl_##name##nbits(unsigned runtime_nbits, void *pa, void *pb, void *pr) JL_NOTSAFEPOINT \
{ \
    c_type a = *(c_type*)pa; \
    c_type b = *(c_type*)pb; \
    *(c_type*)pr = (c_type)OP(a, b); \
}

#define bi_intrinsic_half(OP, name) \
static void jl_##name##16(unsigned runtime_nbits, void *pa, void *pb, void *pr) JL_NOTSAFEPOINT \
{ \
    uint16_t a = *(uint16_t*)pa; \
    uint16_t b = *(uint16_t*)pb; \
    float A = half_to_float(a); \
    float B = half_to_float(b); \
    runtime_nbits = 16; \
    float R = OP(A, B); \
    *(uint16_t*)pr = float_to_half(R); \
}

#define bi_intrinsic_bfloat(OP, name) \
static void jl_##name##bf16(unsigned runtime_nbits, void *pa, void *pb, void *pr) JL_NOTSAFEPOINT \
{ \
    uint16_t a = *(uint16_t*)pa; \
    uint16_t b = *(uint16_t*)pb; \
    float A = bfloat_to_float(a); \
    float B = bfloat_to_float(b); \
    runtime_nbits = 16; \
    float R = OP(A, B); \
    *(uint16_t*)pr = float_to_bfloat(R); \
}

// float or integer inputs, bool output
// OP::Function macro(inputa, inputb)
// name::unique string
// nbits::number of bits
// c_type::c_type corresponding to nbits
#define bool_intrinsic_ctype(OP, name, nbits, c_type) \
static int jl_##name##nbits(unsigned runtime_nbits, void *pa, void *pb) JL_NOTSAFEPOINT \
{ \
    c_type a = *(c_type*)pa; \
    c_type b = *(c_type*)pb; \
    return OP(a, b); \
}

#define bool_intrinsic_half(OP, name) \
static int jl_##name##16(unsigned runtime_nbits, void *pa, void *pb) JL_NOTSAFEPOINT \
{ \
    uint16_t a = *(uint16_t*)pa; \
    uint16_t b = *(uint16_t*)pb; \
    float A = half_to_float(a); \
    float B = half_to_float(b); \
    runtime_nbits = 16; \
    return OP(A, B); \
}

#define bool_intrinsic_bfloat(OP, name) \
static int jl_##name##bf16(unsigned runtime_nbits, void *pa, void *pb) JL_NOTSAFEPOINT \
{ \
    uint16_t a = *(uint16_t*)pa; \
    uint16_t b = *(uint16_t*)pb; \
    float A = bfloat_to_float(a); \
    float B = bfloat_to_float(b); \
    runtime_nbits = 16; \
    return OP(A, B); \
}


// integer inputs, with precondition test
// OP::Function macro(inputa, inputb)
// name::unique string
// nbits::number of bits
// c_type::c_type corresponding to nbits
#define checked_intrinsic_ctype(CHECK_OP, OP, name, nbits, c_type) \
static int jl_##name##nbits(unsigned runtime_nbits, void *pa, void *pb, void *pr) JL_NOTSAFEPOINT \
{ \
    c_type a = *(c_type*)pa; \
    c_type b = *(c_type*)pb; \
    *(c_type*)pr = (c_type)OP(a, b); \
    return CHECK_OP(c_type, a, b);    \
}

// float inputs
// OP::Function macro(inputa, inputb, inputc)
// name::unique string
// nbits::number of bits
// c_type::c_type corresponding to nbits
#define ter_intrinsic_ctype(OP, name, nbits, c_type) \
static void jl_##name##nbits(unsigned runtime_nbits, void *pa, void *pb, void *pc, void *pr) JL_NOTSAFEPOINT \
{ \
    c_type a = *(c_type*)pa; \
    c_type b = *(c_type*)pb; \
    c_type c = *(c_type*)pc; \
    *(c_type*)pr = (c_type)OP(a, b, c); \
}

#define ter_intrinsic_half(OP, name) \
static void jl_##name##16(unsigned runtime_nbits, void *pa, void *pb, void *pc, void *pr) JL_NOTSAFEPOINT \
{ \
    uint16_t a = *(uint16_t*)pa; \
    uint16_t b = *(uint16_t*)pb; \
    uint16_t c = *(uint16_t*)pc; \
    float A = half_to_float(a); \
    float B = half_to_float(b); \
    float C = half_to_float(c); \
    runtime_nbits = 16; \
    float R = OP(A, B, C); \
    *(uint16_t*)pr = float_to_half(R); \
}

#define ter_intrinsic_bfloat(OP, name) \
static void jl_##name##bf16(unsigned runtime_nbits, void *pa, void *pb, void *pc, void *pr) JL_NOTSAFEPOINT \
{ \
    uint16_t a = *(uint16_t*)pa; \
    uint16_t b = *(uint16_t*)pb; \
    uint16_t c = *(uint16_t*)pc; \
    float A = bfloat_to_float(a); \
    float B = bfloat_to_float(b); \
    float C = bfloat_to_float(c); \
    runtime_nbits = 16; \
    float R = OP(A, B, C); \
    *(uint16_t*)pr = float_to_bfloat(R); \
}


// unary operator generator //

typedef void (*intrinsic_1_t)(unsigned, void*, void*);
SELECTOR_FUNC(intrinsic_1)
#define un_iintrinsic(name, u) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a) \
{ \
    return jl_iintrinsic_1(a, #name, u##signbitbyte, jl_intrinsiclambda_ty1, name##_list); \
}
#define un_iintrinsic_fast(LLVMOP, OP, name, u) \
un_iintrinsic_ctype(OP, name, 8, u##int##8_t) \
un_iintrinsic_ctype(OP, name, 16, u##int##16_t) \
un_iintrinsic_ctype(OP, name, 32, u##int##32_t) \
un_iintrinsic_ctype(OP, name, 64, u##int##64_t) \
static const select_intrinsic_1_t name##_list = { \
    LLVMOP, \
    jl_##name##8, \
    jl_##name##16, \
    jl_##name##32, \
    jl_##name##64, \
}; \
un_iintrinsic(name, u)
#define un_iintrinsic_slow(LLVMOP, name, u) \
static const select_intrinsic_1_t name##_list = { \
    LLVMOP \
}; \
un_iintrinsic(name, u)

typedef unsigned (*intrinsic_u1_t)(unsigned, void*);
SELECTOR_FUNC(intrinsic_u1)
#define uu_iintrinsic(name, u) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a) \
{ \
    return jl_iintrinsic_1(a, #name, u##signbitbyte, jl_intrinsiclambda_u1, name##_list); \
}
#define uu_iintrinsic_fast(LLVMOP, OP, name, u) \
uu_iintrinsic_ctype(OP, name, 8, u##int##8_t) \
uu_iintrinsic_ctype(OP, name, 16, u##int##16_t) \
uu_iintrinsic_ctype(OP, name, 32, u##int##32_t) \
uu_iintrinsic_ctype(OP, name, 64, u##int##64_t) \
static const select_intrinsic_u1_t name##_list = { \
    LLVMOP, \
    jl_##name##8, \
    jl_##name##16, \
    jl_##name##32, \
    jl_##name##64, \
}; \
uu_iintrinsic(name, u)
#define uu_iintrinsic_slow(LLVMOP, name, u) \
static const select_intrinsic_u1_t name##_list = { \
    LLVMOP \
}; \
uu_iintrinsic(name, u)

static inline
jl_value_t *jl_iintrinsic_1(jl_value_t *a, const char *name,
                            char (*getsign)(void*, unsigned),
                            jl_value_t *(*lambda1)(jl_value_t*, void*, unsigned, unsigned, const void*), const void *list)
{
    jl_value_t *ty = jl_typeof(a);
    if (!jl_is_primitivetype(ty))
        jl_errorf("%s: value is not a primitive type", name);
    void *pa = jl_data_ptr(a);
    unsigned isize = jl_datatype_size(jl_typeof(a));
    unsigned isize2 = next_power_of_two(isize);
    unsigned osize = jl_datatype_size(ty);
    unsigned osize2 = next_power_of_two(osize);
    if (isize2 > osize2)
        osize2 = isize2;
    if (osize2 > isize || isize2 > isize) {
        /* if needed, round type up to a real c-type and set/clear the unused bits */
        void *pa2;
        pa2 = alloca(osize2);
        /* TODO: this memcpy assumes little-endian,
         * for big-endian, need to align the copy to the other end */ \
        memcpy(pa2, pa, isize);
        memset((char*)pa2 + isize, getsign(pa, isize), osize2 - isize);
        pa = pa2;
    }
    jl_value_t *newv = lambda1(ty, pa, osize, osize2, list);
    if (ty == (jl_value_t*)jl_bool_type)
        return *(uint8_t*)jl_data_ptr(newv) & 1 ? jl_true : jl_false;
    return newv;
}

static inline jl_value_t *jl_intrinsiclambda_ty1(jl_value_t *ty, void *pa, unsigned osize, unsigned osize2, const void *voidlist)
{
    intrinsic_1_t op = select_intrinsic_1(osize2, (const intrinsic_1_t*)voidlist);
    void *pr = alloca(osize2);
    op(osize * host_char_bit, pa, pr);
    return jl_new_bits(ty, pr);
}

static inline jl_value_t *jl_intrinsiclambda_u1(jl_value_t *ty, void *pa, unsigned osize, unsigned osize2, const void *voidlist)
{
    jl_task_t *ct = jl_current_task;
    intrinsic_u1_t op = select_intrinsic_u1(osize2, (const intrinsic_u1_t*)voidlist);
    uint64_t cnt = op(osize * host_char_bit, pa);
    // TODO: the following assume little-endian
    // for big-endian, need to copy from the other end of cnt
    if (osize <= sizeof(cnt)) {
        return jl_new_bits(ty, &cnt);
    }
    jl_value_t *newv = jl_gc_alloc(ct->ptls, osize, ty);
    // perform zext, if needed
    memset((char*)jl_data_ptr(newv) + sizeof(cnt), 0, osize - sizeof(cnt));
    memcpy(jl_data_ptr(newv), &cnt, sizeof(cnt));
    return newv;
}

// conversion operator

typedef void (*intrinsic_cvt_t)(jl_datatype_t*, void*, jl_datatype_t*, void*);
typedef unsigned (*intrinsic_cvt_check_t)(unsigned, unsigned, void*);
#define cvt_iintrinsic(LLVMOP, name) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *ty, jl_value_t *a) \
{ \
    return jl_intrinsic_cvt(ty, a, #name, LLVMOP); \
}

static inline jl_value_t *jl_intrinsic_cvt(jl_value_t *ty, jl_value_t *a, const char *name, intrinsic_cvt_t op)
{
    JL_TYPECHKS(name, datatype, ty);
    if (!jl_is_concrete_type(ty) || !jl_is_primitivetype(ty))
        jl_errorf("%s: target type not a leaf primitive type", name);
    jl_value_t *aty = jl_typeof(a);
    if (!jl_is_primitivetype(aty))
        jl_errorf("%s: value is not a primitive type", name);
    void *pa = jl_data_ptr(a);
    unsigned osize = jl_datatype_size(ty);
    void *pr = alloca(osize);
    op((jl_datatype_t*)aty, pa, (jl_datatype_t*)ty, pr);
    return jl_new_bits(ty, pr);
}

// floating point

#define un_fintrinsic_withtype(OP, name) \
un_fintrinsic_bfloat(OP, jl_##name##bf16) \
un_fintrinsic_half(OP, jl_##name##16) \
un_fintrinsic_ctype(OP, jl_##name##32, float) \
un_fintrinsic_ctype(OP, jl_##name##64, double) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *ty, jl_value_t *a) \
{ \
    return jl_fintrinsic_1(ty, a, #name, jl_##name##bf16, jl_##name##16, jl_##name##32, jl_##name##64); \
}

#define un_fintrinsic(OP, name) \
un_fintrinsic_withtype(OP, name##_withtype) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a) \
{ \
    return jl_##name##_withtype(jl_typeof(a), a); \
}

typedef void (fintrinsic_op1)(unsigned, jl_value_t*, void*, void*);

static inline jl_value_t *jl_fintrinsic_1(jl_value_t *ty, jl_value_t *a, const char *name, fintrinsic_op1 *bfloatop, fintrinsic_op1 *halfop, fintrinsic_op1 *floatop, fintrinsic_op1 *doubleop)
{
    jl_task_t *ct = jl_current_task;
    jl_datatype_t *aty = (jl_datatype_t *)jl_typeof(a);
    if (!jl_is_primitivetype(aty))
        jl_errorf("%s: value is not a primitive type", name);
    if (!jl_is_primitivetype(ty))
        jl_errorf("%s: type is not a primitive type", name);
    unsigned sz2 = jl_datatype_size(ty);
    jl_value_t *newv = jl_gc_alloc(ct->ptls, sz2, ty);
    void *pa = jl_data_ptr(a), *pr = jl_data_ptr(newv);

    if (aty == jl_float16_type)
        halfop(sz2 * host_char_bit, ty, pa, pr);
    else if (aty == jl_bfloat16_type)
        bfloatop(sz2 * host_char_bit, ty, pa, pr);
    else if (aty == jl_float32_type)
        floatop(sz2 * host_char_bit, ty, pa, pr);
    else if (aty == jl_float64_type)
        doubleop(sz2 * host_char_bit, ty, pa, pr);
    else
        jl_errorf("%s: runtime floating point intrinsics require both arguments to be Float16, BFloat16, Float32, or Float64", name);

    return newv;
}

// binary operator generator //

// integer

typedef void (*intrinsic_2_t)(unsigned, void*, void*, void*);
SELECTOR_FUNC(intrinsic_2)
#define bi_iintrinsic(name, u, cvtb) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a, jl_value_t *b) \
{ \
    return jl_iintrinsic_2(a, b, #name, u##signbitbyte, jl_intrinsiclambda_2, name##_list, cvtb); \
}
#define bi_iintrinsic_cnvtb_fast(LLVMOP, OP, name, u, cvtb) \
bi_intrinsic_ctype(OP, name, 8, u##int##8_t) \
bi_intrinsic_ctype(OP, name, 16, u##int##16_t) \
bi_intrinsic_ctype(OP, name, 32, u##int##32_t) \
bi_intrinsic_ctype(OP, name, 64, u##int##64_t) \
static const select_intrinsic_2_t name##_list = { \
    LLVMOP, \
    jl_##name##8, \
    jl_##name##16, \
    jl_##name##32, \
    jl_##name##64, \
}; \
bi_iintrinsic(name, u, cvtb)
#define bi_iintrinsic_fast(LLVMOP, OP, name, u) \
    bi_iintrinsic_cnvtb_fast(LLVMOP, OP, name, u, 0)

typedef int (*intrinsic_cmp_t)(unsigned, void*, void*);
SELECTOR_FUNC(intrinsic_cmp)
#define cmp_iintrinsic(name, u) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a, jl_value_t *b) \
{ \
    return jl_iintrinsic_2(a, b, #name, u##signbitbyte, jl_intrinsiclambda_cmp, name##_list, 0); \
}
#define bool_iintrinsic_fast(LLVMOP, OP, name, u) \
bool_intrinsic_ctype(OP, name, 8, u##int##8_t) \
bool_intrinsic_ctype(OP, name, 16, u##int##16_t) \
bool_intrinsic_ctype(OP, name, 32, u##int##32_t) \
bool_intrinsic_ctype(OP, name, 64, u##int##64_t) \
static const select_intrinsic_cmp_t name##_list = { \
    LLVMOP, \
    jl_##name##8, \
    jl_##name##16, \
    jl_##name##32, \
    jl_##name##64, \
}; \
cmp_iintrinsic(name, u)

typedef int (*intrinsic_checked_t)(unsigned, void*, void*, void*) JL_NOTSAFEPOINT;
SELECTOR_FUNC(intrinsic_checked)
#define checked_iintrinsic(name, u, lambda_checked) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a, jl_value_t *b) \
{ \
    return jl_iintrinsic_2(a, b, #name, u##signbitbyte, lambda_checked, name##_list, 0); \
}
#define checked_iintrinsic_fast(LLVMOP, CHECK_OP, OP, name, u) \
checked_intrinsic_ctype(CHECK_OP, OP, name, 8, u##int##8_t) \
checked_intrinsic_ctype(CHECK_OP, OP, name, 16, u##int##16_t) \
checked_intrinsic_ctype(CHECK_OP, OP, name, 32, u##int##32_t) \
checked_intrinsic_ctype(CHECK_OP, OP, name, 64, u##int##64_t) \
static const select_intrinsic_checked_t name##_list = { \
    LLVMOP, \
    jl_##name##8, \
    jl_##name##16, \
    jl_##name##32, \
    jl_##name##64, \
}; \
checked_iintrinsic(name, u, jl_intrinsiclambda_checked)
#define checked_iintrinsic_slow(LLVMOP, name, u) \
static const select_intrinsic_checked_t name##_list = { \
    LLVMOP \
}; \
checked_iintrinsic(name, u, jl_intrinsiclambda_checked)
#define checked_iintrinsic_div(LLVMOP, name, u) \
static const select_intrinsic_checked_t name##_list = { \
    LLVMOP \
}; \
checked_iintrinsic(name, u, jl_intrinsiclambda_checkeddiv)

static inline
jl_value_t *jl_iintrinsic_2(jl_value_t *a, jl_value_t *b, const char *name,
                            char (*getsign)(void*, unsigned),
                            jl_value_t *(*lambda2)(jl_value_t*, void*, void*, unsigned, unsigned, const void*),
                            const void *list, int cvtb)
{
    jl_value_t *ty = jl_typeof(a);
    jl_value_t *tyb = jl_typeof(b);
    if (tyb != ty) {
        if (!cvtb)
            jl_errorf("%s: types of a and b must match", name);
        if (!jl_is_primitivetype(tyb))
            jl_errorf("%s: b is not a primitive type", name);
    }
    if (!jl_is_primitivetype(ty))
        jl_errorf("%s: a is not a primitive type", name);
    void *pa = jl_data_ptr(a), *pb = jl_data_ptr(b);
    unsigned sz = jl_datatype_size(ty);
    unsigned sz2 = next_power_of_two(sz);
    unsigned szb = cvtb ? jl_datatype_size(tyb) : sz;
    if (sz2 > sz) {
        /* round type up to the appropriate c-type and set/clear the unused bits */
        void *pa2 = alloca(sz2);
        memcpy(pa2, pa, sz);
        memset((char*)pa2 + sz, getsign(pa, sz), sz2 - sz);
        pa = pa2;
    }
    if (sz2 > szb) {
        /* round type up to the appropriate c-type and set/clear/truncate the unused bits
         * (zero-extend if cvtb is set, since in that case b is unsigned while the sign of a comes from the op)
         */
        void *pb2 = alloca(sz2);
        memcpy(pb2, pb, szb);
        memset((char*)pb2 + szb, cvtb ? 0 : getsign(pb, szb), sz2 - szb);
        pb = pb2;
    }
    jl_value_t *newv = lambda2(ty, pa, pb, sz, sz2, list);
    return newv;
}

static inline jl_value_t *jl_intrinsiclambda_2(jl_value_t *ty, void *pa, void *pb, unsigned sz, unsigned sz2, const void *voidlist)
{
    void *pr = alloca(sz2);
    intrinsic_2_t op = select_intrinsic_2(sz2, (const intrinsic_2_t*)voidlist);
    op(sz * host_char_bit, pa, pb, pr);
    return jl_new_bits(ty, pr);
}

static inline jl_value_t *jl_intrinsiclambda_cmp(jl_value_t *ty, void *pa, void *pb, unsigned sz, unsigned sz2, const void *voidlist)
{
    intrinsic_cmp_t op = select_intrinsic_cmp(sz2, (const intrinsic_cmp_t*)voidlist);
    int cmp = op(sz * host_char_bit, pa, pb);
    return cmp ? jl_true : jl_false;
}

static inline jl_value_t *jl_intrinsiclambda_checked(jl_value_t *ty, void *pa, void *pb, unsigned sz, unsigned sz2, const void *voidlist)
{
    jl_value_t *params[2];
    params[0] = ty;
    params[1] = (jl_value_t*)jl_bool_type;
    jl_datatype_t *tuptyp = (jl_datatype_t*)jl_apply_tuple_type_v(params, 2);
    JL_GC_PROMISE_ROOTED(tuptyp); // (JL_ALWAYS_LEAFTYPE)
    jl_task_t *ct = jl_current_task;
    jl_value_t *newv = jl_gc_alloc(ct->ptls, jl_datatype_size(tuptyp), tuptyp);

    intrinsic_checked_t op = select_intrinsic_checked(sz2, (const intrinsic_checked_t*)voidlist);
    int ovflw = op(sz * host_char_bit, pa, pb, jl_data_ptr(newv));

    char *ao = (char*)jl_data_ptr(newv) + sz;
    *ao = (char)ovflw;
    return newv;
}
static inline jl_value_t *jl_intrinsiclambda_checkeddiv(jl_value_t *ty, void *pa, void *pb, unsigned sz, unsigned sz2, const void *voidlist)
{
    void *pr = alloca(sz2);
    intrinsic_checked_t op = select_intrinsic_checked(sz2, (const intrinsic_checked_t*)voidlist);
    int ovflw = op(sz * host_char_bit, pa, pb, pr);
    if (ovflw)
        jl_throw(jl_diverror_exception);
    return jl_new_bits(ty, pr);
}

// floating point

#define bi_fintrinsic(OP, name) \
    bi_intrinsic_bfloat(OP, name) \
    bi_intrinsic_half(OP, name) \
    bi_intrinsic_ctype(OP, name, 32, float) \
    bi_intrinsic_ctype(OP, name, 64, double) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a, jl_value_t *b) \
{ \
    jl_task_t *ct = jl_current_task; \
    jl_value_t *ty = jl_typeof(a); \
    jl_datatype_t *aty = (jl_datatype_t *)ty; \
    if (jl_typeof(b) != ty) \
        jl_error(#name ": types of a and b must match"); \
    if (!jl_is_primitivetype(ty)) \
        jl_error(#name ": values are not primitive types"); \
    int sz = jl_datatype_size(ty); \
    jl_value_t *newv = jl_gc_alloc(ct->ptls, sz, ty); \
    void *pa = jl_data_ptr(a), *pb = jl_data_ptr(b), *pr = jl_data_ptr(newv); \
    if (aty == jl_float16_type) \
        jl_##name##16(16, pa, pb, pr); \
    else if (aty == jl_bfloat16_type) \
        jl_##name##bf16(16, pa, pb, pr); \
    else if (aty == jl_float32_type) \
        jl_##name##32(32, pa, pb, pr); \
    else if (aty == jl_float64_type) \
        jl_##name##64(64, pa, pb, pr); \
    else \
        jl_error(#name ": runtime floating point intrinsics require both arguments to be Float16, BFloat16, Float32, or Float64"); \
    return newv; \
}

#define bool_fintrinsic(OP, name) \
    bool_intrinsic_bfloat(OP, name) \
    bool_intrinsic_half(OP, name) \
    bool_intrinsic_ctype(OP, name, 32, float) \
    bool_intrinsic_ctype(OP, name, 64, double) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a, jl_value_t *b) \
{ \
    jl_value_t *ty = jl_typeof(a); \
    jl_datatype_t *aty = (jl_datatype_t *)ty; \
    if (jl_typeof(b) != ty) \
        jl_error(#name ": types of a and b must match"); \
    if (!jl_is_primitivetype(ty)) \
        jl_error(#name ": values are not primitive types"); \
    void *pa = jl_data_ptr(a), *pb = jl_data_ptr(b); \
    int cmp; \
    if (aty == jl_float16_type) \
        cmp = jl_##name##16(16, pa, pb); \
    else if (aty == jl_bfloat16_type) \
        cmp = jl_##name##bf16(16, pa, pb); \
    else if (aty == jl_float32_type) \
        cmp = jl_##name##32(32, pa, pb); \
    else if (aty == jl_float64_type) \
        cmp = jl_##name##64(64, pa, pb); \
    else \
        jl_error(#name ": runtime floating point intrinsics require both arguments to be Float16, BFloat16, Float32, or Float64"); \
 \
    return cmp ? jl_true : jl_false; \
}

#define ter_fintrinsic(OP, name) \
    ter_intrinsic_bfloat(OP, name) \
    ter_intrinsic_half(OP, name) \
    ter_intrinsic_ctype(OP, name, 32, float) \
    ter_intrinsic_ctype(OP, name, 64, double) \
JL_DLLEXPORT jl_value_t *jl_##name(jl_value_t *a, jl_value_t *b, jl_value_t *c) \
{ \
    jl_task_t *ct = jl_current_task; \
    jl_value_t *ty = jl_typeof(a); \
    jl_datatype_t *aty = (jl_datatype_t *)ty; \
    if (jl_typeof(b) != ty || jl_typeof(c) != ty) \
        jl_error(#name ": types of a, b, and c must match"); \
    if (!jl_is_primitivetype(ty)) \
        jl_error(#name ": values are not primitive types"); \
    int sz = jl_datatype_size(ty); \
    jl_value_t *newv = jl_gc_alloc(ct->ptls, sz, ty); \
    void *pa = jl_data_ptr(a), *pb = jl_data_ptr(b), *pc = jl_data_ptr(c), *pr = jl_data_ptr(newv); \
    if (aty == jl_float16_type) \
            jl_##name##16(16, pa, pb, pc, pr); \
    else if (aty == jl_bfloat16_type) \
            jl_##name##bf16(16, pa, pb, pc, pr); \
    else if (aty == jl_float32_type) \
        jl_##name##32(32, pa, pb, pc, pr); \
    else if (aty == jl_float64_type) \
        jl_##name##64(64, pa, pb, pc, pr); \
    else \
        jl_error(#name ": runtime floating point intrinsics require both arguments to be Float16, BFloat16, Float32, or Float64"); \
    return newv; \
}

// arithmetic
#define neg(a) -a
#define neg_float(ty, pr, a) *pr = -a
un_iintrinsic_fast(LLVMNeg, neg, neg_int, u)
#define add(a,b) a + b
bi_iintrinsic_fast(LLVMAdd, add, add_int, u)
#define sub(a,b) a - b
bi_iintrinsic_fast(LLVMSub, sub, sub_int, u)
#define mul(a,b) a * b
bi_iintrinsic_fast(LLVMMul, mul, mul_int, u)
#define div(a,b) a / b
bi_iintrinsic_fast(LLVMSDiv, div, sdiv_int,  )
bi_iintrinsic_fast(LLVMUDiv, div, udiv_int, u)
#define rem(a,b) a % b
bi_iintrinsic_fast(LLVMSRem, rem, srem_int,  )
bi_iintrinsic_fast(LLVMURem, rem, urem_int, u)
#define smod(a,b) ((a < 0) == (b < 0)) ? a % b : (b + (a % b)) % b
bi_iintrinsic_fast(jl_LLVMSMod, smod, smod_int,  )
#define frem(a, b) \
    fp_select2(a, b, fmod)
un_fintrinsic(neg_float,neg_float)
bi_fintrinsic(add,add_float)
bi_fintrinsic(sub,sub_float)
bi_fintrinsic(mul,mul_float)
bi_fintrinsic(div,div_float)

float min_float(float x, float y) JL_NOTSAFEPOINT
{
    float diff = x - y;
    float argmin = signbit(diff) ? x : y;
    int is_nan = isnan(x) || isnan(y);
    return is_nan ? diff : argmin;
}

double min_double(double x, double y) JL_NOTSAFEPOINT
{
    double diff = x - y;
    double argmin = signbit(diff) ? x : y;
    int is_nan = isnan(x) || isnan(y);
    return is_nan ? diff : argmin;
}

#define _min(a, b) sizeof(a) == sizeof(float) ? min_float(a, b) : min_double(a, b)
bi_fintrinsic(_min, min_float)

float max_float(float x, float y) JL_NOTSAFEPOINT
{
    float diff = x - y;
    float argmax = signbit(diff) ? y : x;
    int is_nan = isnan(x) || isnan(y);
    return is_nan ? diff : argmax;
}

double max_double(double x, double y) JL_NOTSAFEPOINT
{
    double diff = x - y;
    double argmax = signbit(diff) ? y : x;
    int is_nan = isnan(x) || isnan(y);
    return is_nan ? diff : argmax;
}

#define _max(a, b) sizeof(a) == sizeof(float) ? max_float(a, b) : max_double(a, b)
bi_fintrinsic(_max, max_float)

// ternary operators //
// runtime fma is broken on windows, define julia_fma(f) ourself with fma_emulated as reference.
#if defined(_OS_WINDOWS_)
// reinterpret(UInt64, ::Float64)
uint64_t bitcast_d2u(double d) {
    uint64_t r;
    memcpy(&r, &d, 8);
    return r;
}
// reinterpret(Float64, ::UInt64)
double bitcast_u2d(uint64_t d) {
    double r;
    memcpy(&r, &d, 8);
    return r;
}
// Base.splitbits(::Float64)
void splitbits(double *hi, double *lo, double d) {
    *hi = bitcast_u2d(bitcast_d2u(d) & 0xfffffffff8000000);
    *lo = d - *hi;
}
// Base.exponent(::Float64)
int exponent(double a) {
    int e;
    frexp(a, &e);
    return e - 1;
}
// Base.fma_emulated(::Float32, ::Float32, ::Float32)
float julia_fmaf(float a, float b, float c) {
    double ab, res;
    ab = (double)a * b;
    res = ab + (double)c;
    if ((bitcast_d2u(res) & 0x1fffffff) == 0x10000000){
        double reslo = fabsf(c) > fabs(ab) ? ab-(res - c) : c-(res - ab);
        if (reslo != 0)
            res = nextafter(res, copysign(1.0/0.0, reslo));
    }
    return (float)res;
}
// Base.twomul(::Float64, ::Float64)
void two_mul(double *abhi, double *ablo, double a, double b) {
    double ahi, alo, bhi, blo, blohi, blolo;
    splitbits(&ahi, &alo, a);
    splitbits(&bhi, &blo, b);
    splitbits(&blohi, &blolo, blo);
    *abhi = a*b;
    *ablo = alo*blohi - (((*abhi - ahi*bhi) - alo*bhi) - ahi*blo) + blolo*alo;
}
// Base.issubnormal(::Float64) (Win32's fpclassify seems broken)
int issubnormal(double d) {
    uint64_t y = bitcast_d2u(d);
    return ((y & 0x7ff0000000000000) == 0) & ((y & 0x000fffffffffffff) != 0);
}
#if defined(_WIN32)
// Win32 needs volatile (avoid over optimization?)
#define VDOUBLE volatile double
#else
#define VDOUBLE double
#endif

// Base.fma_emulated(::Float64, ::Float64, ::Float64)
double julia_fma(double a, double b, double c) {
    double abhi, ablo, r, s;
    two_mul(&abhi, &ablo, a, b);
    if (!isfinite(abhi+c) || fabs(abhi) < 2.0041683600089732e-292 ||
        issubnormal(a) || issubnormal(b)) {
        int aandbfinite = isfinite(a) && isfinite(b);
        if (!(aandbfinite && isfinite(c)))
            return aandbfinite ? c : abhi+c;
        if (a == 0 || b == 0)
            return abhi+c;
        int bias = exponent(a) + exponent(b);
        VDOUBLE c_denorm = ldexp(c, -bias);
        if (isfinite(c_denorm)) {
            if (issubnormal(a))
                a *= 4.503599627370496e15;
            if (issubnormal(b))
                b *= 4.503599627370496e15;
            a = bitcast_u2d((bitcast_d2u(a) & 0x800fffffffffffff) | 0x3ff0000000000000);
            b = bitcast_u2d((bitcast_d2u(b) & 0x800fffffffffffff) | 0x3ff0000000000000);
            c = c_denorm;
            two_mul(&abhi, &ablo, a, b);
            r = abhi+c;
            s = (fabs(abhi) > fabs(c)) ? (abhi-r+c+ablo) : (c-r+abhi+ablo);
            double sumhi = r+s;
            if (issubnormal(ldexp(sumhi, bias))) {
                double sumlo = r-sumhi+s;
                int bits_lost = -bias-exponent(sumhi)-1022;
                if ((bits_lost != 1) ^ ((bitcast_d2u(sumhi)&1) == 1))
                    if (sumlo != 0)
                        sumhi = nextafter(sumhi, copysign(1.0/0.0, sumlo));
            }
            return ldexp(sumhi, bias);
        }
        if (isinf(abhi) && signbit(c) == signbit(a*b))
            return abhi;
    }
    r = abhi+c;
    s = (fabs(abhi) > fabs(c)) ? (abhi-r+c+ablo) : (c-r+abhi+ablo);
    return r+s;
}
#define fma(a, b, c) \
    sizeof(a) == sizeof(float) ? julia_fmaf(a, b, c) : julia_fma(a, b, c)
#else // On other systems use fma(f) directly
#define fma(a, b, c) \
    sizeof(a) == sizeof(float) ? fmaf(a, b, c) : fma(a, b, c)
#endif

#define muladd(a, b, c) a * b + c
ter_fintrinsic(fma,fma_float)
ter_fintrinsic(muladd,muladd_float)

// same-type comparisons
#define eq(a,b) a == b
bool_iintrinsic_fast(LLVMICmpEQ, eq, eq_int, u)
#define ne(a,b) a != b
bool_iintrinsic_fast(LLVMICmpNE, ne, ne_int, u)
#define lt(a,b) a < b
bool_iintrinsic_fast(LLVMICmpSLT, lt, slt_int,  )
bool_iintrinsic_fast(LLVMICmpULT, lt, ult_int, u)
#define le(a,b) a <= b
bool_iintrinsic_fast(LLVMICmpSLE, le, sle_int,  )
bool_iintrinsic_fast(LLVMICmpULE, le, ule_int, u)

typedef union {
    float f;
    int32_t d;
    uint32_t ud;
} bits32;
typedef union {
    double f;
    int64_t d;
    uint64_t ud;
} bits64;

#define fpiseq_n(c_type, nbits) \
static inline int fpiseq##nbits(c_type a, c_type b) JL_NOTSAFEPOINT { \
    bits##nbits ua, ub; \
    ua.f = a; \
    ub.f = b; \
    return (isnan(a) && isnan(b)) || ua.d == ub.d; \
}
fpiseq_n(float, 32)
fpiseq_n(double, 64)
#define fpiseq(a,b) \
    sizeof(a) <= sizeof(float) ? fpiseq32(a, b) : fpiseq64(a, b)

bool_fintrinsic(eq,eq_float)
bool_fintrinsic(ne,ne_float)
bool_fintrinsic(lt,lt_float)
bool_fintrinsic(le,le_float)
bool_fintrinsic(fpiseq,fpiseq)

// bitwise operators
#define and_op(a,b) a & b
bi_iintrinsic_fast(LLVMAnd, and_op, and_int, u)
#define or_op(a,b) a | b
bi_iintrinsic_fast(LLVMOr, or_op, or_int, u)
#define xor_op(a,b) a ^ b
bi_iintrinsic_fast(LLVMXor, xor_op, xor_int, u)
#define shl_op(a,b) b >= 8 * sizeof(a) ? 0 : a << b
bi_iintrinsic_cnvtb_fast(LLVMShl, shl_op, shl_int, u, 1)
#define lshr_op(a,b) (b >= 8 * sizeof(a)) ? 0 : a >> b
bi_iintrinsic_cnvtb_fast(LLVMLShr, lshr_op, lshr_int, u, 1)
#define ashr_op(a,b) ((b < 0 || b >= 8 * sizeof(a)) ? a >> (8 * sizeof(a) - 1) : a >> b)
bi_iintrinsic_cnvtb_fast(LLVMAShr, ashr_op, ashr_int, , 1)
//#define bswap_op(a) __builtin_bswap(a)
//un_iintrinsic_fast(LLVMByteSwap, bswap_op, bswap_int, u)
un_iintrinsic_slow(LLVMByteSwap, bswap_int, u)
//#define ctpop_op(a) __builtin_ctpop(a)
//uu_iintrinsic_fast(LLVMPopcount, ctpop_op, ctpop_int, u)
uu_iintrinsic_slow(LLVMPopcount, ctpop_int, u)
//#define ctlz_op(a) __builtin_ctlz(a)
//uu_iintrinsic_fast(LLVMCountl_zero, ctlz_op, ctlz_int, u)
uu_iintrinsic_slow(LLVMCountl_zero, ctlz_int, u)
//#define cttz_op(a) __builtin_cttz(a)
//uu_iintrinsic_fast(LLVMCountr_zero, cttz_op, cttz_int, u)
uu_iintrinsic_slow(LLVMCountr_zero, cttz_int, u)
#define not_op(a) ~a
un_iintrinsic_fast(LLVMFlipAllBits, not_op, not_int, u)

// conversions
cvt_iintrinsic(LLVMTrunc, trunc_int)
cvt_iintrinsic(LLVMSExt, sext_int)
cvt_iintrinsic(LLVMZExt, zext_int)
cvt_iintrinsic(LLVMSItoFP, sitofp)
cvt_iintrinsic(LLVMUItoFP, uitofp)
cvt_iintrinsic(LLVMFPtoSI, fptosi)
cvt_iintrinsic(LLVMFPtoUI, fptoui)

#define fintrinsic_read_float16(p)   half_to_float(*(uint16_t *)p)
#define fintrinsic_read_bfloat16(p)  bfloat_to_float(*(uint16_t *)p)
#define fintrinsic_read_float32(p)   *(float *)p
#define fintrinsic_read_float64(p)   *(double *)p

#define fintrinsic_write_float16(p, x)  *(uint16_t *)p = double_to_half(x)
#define fintrinsic_write_bfloat16(p, x) *(uint16_t *)p = double_to_bfloat(x)
#define fintrinsic_write_float32(p, x)  *(float *)p = x
#define fintrinsic_write_float64(p, x)  *(double *)p = x

/*
 * aty: Type of value argument (input)
 * pa:  Pointer to value argument data
 * ty:  Type argument (output)
 * pr:  Pointer to result data
 */

static inline void fptrunc(jl_datatype_t *aty, void *pa, jl_datatype_t *ty, void *pr)
{
    unsigned isize = jl_datatype_size(aty), osize = jl_datatype_size(ty);
    if (!(osize < isize)) {
        jl_error("fptrunc: output bitsize must be < input bitsize");
        return;
    }

#define fptrunc_convert(in, out)                                \
    else if (aty == jl_##in##_type && ty == jl_##out##_type)    \
        fintrinsic_write_##out(pr, fintrinsic_read_##in(pa))

    if (0)
        ;
    fptrunc_convert(float32, float16);
    fptrunc_convert(float64, float16);
    fptrunc_convert(float32, bfloat16);
    fptrunc_convert(float64, bfloat16);
    fptrunc_convert(float64, float32);
    else
        jl_error("fptrunc: runtime floating point intrinsics require both arguments to be Float16, BFloat16, Float32, or Float64");
#undef fptrunc_convert
}

static inline void fpext(jl_datatype_t *aty, void *pa, jl_datatype_t *ty, void *pr)
{
    unsigned isize = jl_datatype_size(aty), osize = jl_datatype_size(ty);
    if (!(osize > isize)) {
        jl_error("fpext: output bitsize must be > input bitsize");
        return;
    }

#define fpext_convert(in, out)                                  \
    else if (aty == jl_##in##_type && ty == jl_##out##_type)    \
        fintrinsic_write_##out(pr, fintrinsic_read_##in(pa))

    if (0)
        ;
    fpext_convert(float16, float32);
    fpext_convert(float16, float64);
    fpext_convert(bfloat16, float32);
    fpext_convert(bfloat16, float64);
    fpext_convert(float32, float64);
    else
        jl_error("fptrunc: runtime floating point intrinsics require both arguments to be Float16, BFloat16, Float32, or Float64");
#undef fpext_convert
}

cvt_iintrinsic(fptrunc, fptrunc)
cvt_iintrinsic(fpext, fpext)


// checked arithmetic
/**
 * s_typemin = - s_typemax - 1
 * s_typemax = ((t)1 << (runtime_nbits - 1)) - 1
 * u_typemin = 0
 * u_typemax = ((t)1 << runtime_nbits) - 1
 **/
#define sTYPEMIN(t) -sTYPEMAX(t) - 1
#define sTYPEMAX(t)                                                \
    ((t)(8 * sizeof(a) == runtime_nbits                            \
         ? ((((((t)1) << (8 * sizeof(t) - 2)) - 1) << 1) + 1)      \
         : (  (((t)1) << (runtime_nbits - 1)) - 1)))

#define uTYPEMIN(t) ((t)0)
#define uTYPEMAX(t)                                             \
    ((t)(8 * sizeof(t) == runtime_nbits                         \
         ? (~((t)0)) : (~(((t)~((t)0)) << runtime_nbits))))
#define check_sadd_int(t, a, b)                                         \
        /* this test checks for (b >= 0) ? (a + b > typemax) : (a + b < typemin) ==> overflow */ \
        (b >= 0) ? (a > sTYPEMAX(t) - b) : (a < sTYPEMIN(t) - b)
checked_iintrinsic_fast(LLVMAdd_sov, check_sadd_int, add, checked_sadd_int,  )
#define check_uadd_int(t, a, b)                                       \
    /* this test checks for (a + b) > typemax(a) ==> overflow */      \
    a > uTYPEMAX(t) - b
checked_iintrinsic_fast(LLVMAdd_uov, check_uadd_int, add, checked_uadd_int, u)
#define check_ssub_int(t, a, b)                                         \
    /* this test checks for (b >= 0) ? (a - b < typemin) : (a - b > typemax) ==> overflow */ \
    (b >= 0) ? (a < sTYPEMIN(t) + b) : (a > sTYPEMAX(t) + b)
checked_iintrinsic_fast(LLVMSub_sov, check_ssub_int, sub, checked_ssub_int,  )
#define check_usub_int(t, a, b)                                   \
    /* this test checks for (a - b) < typemin ==> overflow */     \
    a < uTYPEMIN(t) + b
checked_iintrinsic_fast(LLVMSub_uov, check_usub_int, sub, checked_usub_int, u)
checked_iintrinsic_slow(LLVMMul_sov, checked_smul_int,  )
checked_iintrinsic_slow(LLVMMul_uov, checked_umul_int, u)

checked_iintrinsic_div(LLVMDiv_sov, checked_sdiv_int,  )
checked_iintrinsic_div(LLVMDiv_uov, checked_udiv_int, u)
checked_iintrinsic_div(LLVMRem_sov, checked_srem_int,  )
checked_iintrinsic_div(LLVMRem_uov, checked_urem_int, u)

// functions
#define flipsign(a, b) \
        (b >= 0) ? a : -a
bi_iintrinsic_fast(jl_LLVMFlipSign, flipsign, flipsign_int,  )
#define abs_float(ty, pr, a)      *pr = fp_select(a, fabs)
#define ceil_float(ty, pr, a)     *pr = fp_select(a, ceil)
#define floor_float(ty, pr, a)    *pr = fp_select(a, floor)
#define trunc_float(ty, pr, a)    *pr = fp_select(a, trunc)
#define rint_float(ty, pr, a)     *pr = fp_select(a, rint)
#define sqrt_float(ty, pr, a)     *pr = fp_select(a, sqrt)
#define copysign_float(a, b)  fp_select2(a, b, copysign)

un_fintrinsic(abs_float,abs_float)
bi_fintrinsic(copysign_float,copysign_float)
un_fintrinsic(ceil_float,ceil_llvm)
un_fintrinsic(floor_float,floor_llvm)
un_fintrinsic(trunc_float,trunc_llvm)
un_fintrinsic(rint_float,rint_llvm)
un_fintrinsic(sqrt_float,sqrt_llvm)
un_fintrinsic(sqrt_float,sqrt_llvm_fast)
jl_value_t *jl_cpu_has_fma(int bits);

JL_DLLEXPORT jl_value_t *jl_have_fma(jl_value_t *typ)
{
    JL_TYPECHK(have_fma, datatype, typ); // TODO what about float16/bfloat16?
    if (typ == (jl_value_t*)jl_float32_type)
        return jl_cpu_has_fma(32);
    else if (typ == (jl_value_t*)jl_float64_type)
        return jl_cpu_has_fma(64);
    else
        return jl_false;
}

JL_DLLEXPORT jl_value_t *jl_add_ptr(jl_value_t *ptr, jl_value_t *offset)
{
    JL_TYPECHK(add_ptr, pointer, ptr);
    JL_TYPECHK(add_ptr, ulong, offset);
    char *ptrval = (char*)jl_unbox_long(ptr) + jl_unbox_ulong(offset);
    return jl_new_bits(jl_typeof(ptr), &ptrval);
}

JL_DLLEXPORT jl_value_t *jl_sub_ptr(jl_value_t *ptr, jl_value_t *offset)
{
    JL_TYPECHK(sub_ptr, pointer, ptr);
    JL_TYPECHK(sub_ptr, ulong, offset);
    char *ptrval = (char*)jl_unbox_long(ptr) - jl_unbox_ulong(offset);
    return jl_new_bits(jl_typeof(ptr), &ptrval);
}
