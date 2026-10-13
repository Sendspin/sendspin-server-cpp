// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file fixed_lx6.h
/// @brief Xtensa LX6 inline-assembly overrides for fixed-point primitives
///
/// Redefines MULT32_32_Q32 and SATURATE16 using the mulsh and clamps
/// instructions; included by fixed_generic.h for Xtensa (__xtensa__) builds

#ifndef FIXED_LX6_H
#define FIXED_LX6_H

#include <cstdint>

#undef MULT32_32_Q32
static inline int32_t mult32_32_q32_lx6(int32_t a, int32_t b) {
    int32_t res;
    __asm__("mulsh %0, %1, %2\n\t" : "=r"(res) : "r"(a), "r"(b));
    return res;
}
#define MULT32_32_Q32(a, b) mult32_32_q32_lx6(a, b)

#undef SATURATE16
static inline int16_t saturate16_lx6(int32_t x) {
    int16_t res;
    __asm__("clamps %0, %1, 15\n\t" : "=r"(res) : "r"(x));
    return res;
}
#define SATURATE16(x) saturate16_lx6(x)

#endif
