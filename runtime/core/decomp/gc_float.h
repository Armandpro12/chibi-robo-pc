// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ExpansionPak (DolRecomp); Copyright 2009 Dolphin Emulator Project (GPL-2.0-or-later, used here
// under the GPL-3.0)
//
// Single-precision arithmetic of the GameCube CPU (Gekko) for decompiled code (generic GameCube runtime; no reference
// to any specific game).
//
// Provenance: adapted from DolRecomp, commit c876e2b9e022e084aa9a690fe15e2ad79a0706cd, src/cpu/cpu.c
// (GPL-3.0-or-later): force_25_bit (round_multiplier_25 below), correct_single_fma_tie and single_fma_value (the
// half-way correction of fmadds), force_single (the non-IEEE flush), and the NaN rules of binary_fp and ppc_fma.
// Those helpers follow Dolphin, commit e6f3ae17627e4344da95b13424af5baf4c892b08,
// Source/Core/Core/PowerPC/Interpreter/Interpreter_FPUtils.h (Copyright 2009 Dolphin Emulator Project,
// GPL-2.0-or-later): Force25Bit, ForceSingle and NI_madd_msub. Changes: rewritten as header-only C++ functions of
// one instruction each, on values instead of a CPU state (the caller passes the FPSCR mode explicitly), without the
// FPSCR status and exception bits, and with the subnormal case of Force25Bit written as one shifted rounding bit.
//
// Why: a PowerPC single-precision instruction does not compute what the same C++ expression on `float` computes on a
// PC in every case, and the decompiled code must give the recompiled original's results bit for bit (the oracle
// compares them). Each function below is one instruction, with the results of the DolRecomp helpers that the
// recompiled code calls (ppc_fadds, ppc_fsubs, ppc_fmuls, ppc_fma):
//   - every operation is computed in double precision, then rounded once to single (fadds, fsubs, fmuls), which
//     `static_cast<f32>(double)` does on the host in round-to-nearest;
//   - fmuls and the fmadds family first round their frC operand to a 25-bit significand (Force25Bit): `float` * `float`
//     on the host gives other results;
//   - fmadds computes a*c + b with one rounding to double (std::fma), then to single; a result that falls exactly half
//     way between two singles after the first rounding is moved one double step toward the exact value first, so that
//     the second rounding goes the way a single rounding of the exact value would;
//   - NaN results: the first NaN operand, made quiet, in the order a, b (then c for fmadds); otherwise the default
//     NaN 0x7FF8000000000000 (an invalid operation such as inf - inf), not the host's negative default NaN;
//   - non-IEEE mode (FPSCR[NI] = 1, mode::non_ieee): the result of fadds, fsubs and fmuls is flushed to a zero of its
//     sign when its double value, before rounding to single, is below the smallest normal single (2^-126) in
//     magnitude, as DolRecomp's force_single and Dolphin's ForceSingle do. DolRecomp's ppc_fma does not flush the
//     fmadds result (Dolphin's fmaddsx does): fmadds follows DolRecomp, so the mode does not change it.
// Limits (the conditions under which these functions equal DolRecomp's helpers, the oracle's reference; that the
// helpers, the double rounding of fmadds in particular, equal the console in every case is not proven here):
//   - FPSCR as the decompiled code finds it: round to nearest (RN = 0), no exception enabled, and NI as the `mode`
//     argument says. A game that changes RN or enables exceptions needs other helpers [each game checks it: e.g. no
//     mtfsf/mtfsb in its code]; FPSCR's status bits (FPRF, FI, FR, sticky exceptions) are not computed, the oracle
//     does not compare FPSCR. Under NI = 1, subnormal operands are used as they are (DolRecomp and Dolphin do the
//     same: they flush results only). The double-precision instructions (fadd, fmul...), which NI also flushes in
//     DolRecomp (force_double), have no helper here;
//   - operands are the full double-precision register values (pass f64, as the instruction reads them); the result
//     of fmadds on a NaN operand that is not a single-precision value keeps the double NaN in the register, which an
//     f32 result cannot hold [hypothesis: does not happen with operands that come from single-precision loads or
//     operations, whose NaNs are singles].
// Use: f32 x = gc::fp::fmuls(mode, a, c); the order of the operands is the instruction's (frA, frC, frB), not
// commutative.
#ifndef GC_FLOAT_H
#define GC_FLOAT_H

#include <bit>
#include <cmath>
#include <cstdint>

namespace gc::fp {

using f32 = float;
using f64 = double;

// The FPSCR[NI] bit (non-IEEE mode) the code runs with, given by the caller to each helper whose result depends on it
// (there is no global floating-point state in this layer).
enum class mode : std::uint8_t {
    ieee,     // NI = 0
    non_ieee, // NI = 1: single results below the smallest normal single become zeros
};

namespace detail {

inline constexpr std::uint64_t sign_bit = 0x8000000000000000ull;
inline constexpr std::uint64_t exponent_bits = 0x7FF0000000000000ull;
inline constexpr std::uint64_t fraction_bits = 0x000FFFFFFFFFFFFFull;
inline constexpr std::uint64_t quiet_bit = 0x0008000000000000ull;
inline constexpr std::uint64_t default_nan = 0x7FF8000000000000ull;
inline constexpr std::uint64_t smallest_normal_single = 0x3810000000000000ull; // 2^-126 as a double

#if defined(GC_DECOMP_ORACLE) && !defined(GC_FLOAT_COUNT_DIVERGENCES)
#define GC_FLOAT_COUNT_DIVERGENCES 1
#endif
#if defined(GC_FLOAT_COUNT_DIVERGENCES)
// Oracle executable (and tests that define GC_FLOAT_COUNT_DIVERGENCES) only: fmadds results under NI = 1 that Dolphin's
// interpreter and DolRecomp's ppc_fma give differently (see fmadds). The oracle reads it around each decompiled run.
inline std::uint64_t fmadds_dolphin_divergences = 0;
#endif

inline std::uint64_t bits_of(f64 x) noexcept {
    return std::bit_cast<std::uint64_t>(x);
}
inline f64 from_bits(std::uint64_t b) noexcept {
    return std::bit_cast<f64>(b);
}
inline f64 quiet(f64 x) noexcept {
    return from_bits(bits_of(x) | quiet_bit);
}

// NaN result of a two-operand operation: the first NaN operand made quiet, else the default NaN.
inline f64 nan_result(f64 a, f64 b) noexcept {
    if (std::isnan(a))
        return quiet(a);
    if (std::isnan(b))
        return quiet(b);
    return from_bits(default_nan);
}

// The double result of a single-precision operation rounded to single, as the instruction writes it in `m`. Under
// NI = 1 a value below 2^-126 in magnitude (zeros and NaNs aside, which it keeps) becomes a zero of its sign before any
// rounding, even one that would round up to 2^-126. DolRecomp's force_single also flushes a subnormal single after
// the rounding; that cannot happen here, since every value left is at least 2^-126, a single.
inline f32 round_to_single(mode m, f64 value) noexcept {
    if (m == mode::non_ieee) {
        const std::uint64_t b = bits_of(value);
        if ((b & ~sign_bit) < smallest_normal_single)
            return std::bit_cast<f32>(static_cast<std::uint32_t>((b & sign_bit) >> 32));
    }
    return static_cast<f32>(value);
}

} // namespace detail

// frC as fmuls and the fmadds family use it: the significand rounded to 25 bits, half up. For a normal operand the
// bits of the fraction below bit 27 are dropped and bit 27 is added to itself (a set bit 27 carries into bit 28, which
// may reach the exponent; the result always has bit 27 clear). For a subnormal operand the window moves down with
// its leading one; one with a leading one at fraction bit 24 or lower is kept as it is.
inline f64 round_multiplier_25(f64 c) noexcept {
    const std::uint64_t b = detail::bits_of(c);
    const std::uint64_t fraction = b & detail::fraction_bits;
    unsigned round_bit = 27;
    if ((b & detail::exponent_bits) == 0 && fraction != 0) {
        // Subnormal: `below` = how far its leading one is under the implicit bit 52 of a normal number.
        const unsigned below = static_cast<unsigned>(std::countl_zero(fraction)) - 11u;
        if (below >= 28u)
            return c;
        round_bit = 27u - below;
    }
    const std::uint64_t round = std::uint64_t{1} << round_bit;
    return detail::from_bits((b & ~(round - 1u)) + (b & round));
}

// fadds frD, frA, frB
inline f32 fadds(mode m, f64 a, f64 b) noexcept {
    const f64 r = a + b;
    return detail::round_to_single(m, std::isnan(r) ? detail::nan_result(a, b) : r);
}

// fsubs frD, frA, frB
inline f32 fsubs(mode m, f64 a, f64 b) noexcept {
    const f64 r = a - b;
    return detail::round_to_single(m, std::isnan(r) ? detail::nan_result(a, b) : r);
}

// fmuls frD, frA, frC
inline f32 fmuls(mode m, f64 a, f64 c) noexcept {
    const f64 c25 = round_multiplier_25(c);
    const f64 r = a * c25;
    return detail::round_to_single(m, std::isnan(r) ? detail::nan_result(a, c25) : r);
}

// fmadds frD, frA, frC, frB: a * c + b. The mode does not change the result (DolRecomp's ppc_fma has no NI flush; see
// the header); it is taken so that every call says the mode it runs in. Where the two references part (counted when
// GC_FLOAT_COUNT_DIVERGENCES is defined): under NI = 1, Dolphin's fmaddsx passes the double result through
// ForceSingle, which makes a value below 2^-126 in magnitude a zero of its sign; DolRecomp rounds it to single as
// usual. They differ when that rounding does not give zero (Dolphin e6f3ae17 Interpreter_FloatingPoint.cpp fmaddsx and
// Interpreter_FPUtils.h ForceSingle; DolRecomp c876e2b9 cpu.c ppc_fma).
inline f32 fmadds([[maybe_unused]] mode m, f64 a, f64 c, f64 b) noexcept {
    const f64 c25 = round_multiplier_25(c);
    f64 r = std::fma(a, c25, b);
    std::uint64_t rb = detail::bits_of(r);
    // Half way between two singles: the 29 fraction bits a single does not hold are exactly 1 followed by zeros (bit
    // 28 set, bits 0 to 27 clear).
    if ((rb & 0x1FFFFFFFull) == 0x10000000ull) {
        // Sign of the error of r: (a * c25 + b) - r = a * c25 + (b - r), the rounding error of b - r added back.
        const f64 difference = b - r;
        const f64 difference_error = b - (r + difference);
        const f64 error = std::fma(a, c25, difference) + difference_error;
        if (error != 0.0) {
            rb = (error > 0.0) == (r > 0.0) ? rb + 1u : rb - 1u;
            r = detail::from_bits(rb);
        }
    }
    const f64 single = static_cast<f64>(static_cast<f32>(r));
#if defined(GC_FLOAT_COUNT_DIVERGENCES)
    if (m == mode::non_ieee && (rb & ~detail::sign_bit) < detail::smallest_normal_single && single != 0.0)
        detail::fmadds_dolphin_divergences++;
#endif
    if (!std::isnan(single))
        return static_cast<f32>(single);
    if (std::isnan(a))
        return static_cast<f32>(detail::quiet(a));
    if (std::isnan(b))
        return static_cast<f32>(detail::quiet(b));
    if (std::isnan(c))
        return static_cast<f32>(detail::quiet(c));
    return static_cast<f32>(detail::from_bits(detail::default_nan));
}

// fneg frD, frB: the sign bit flipped, nothing else (also for a NaN; no rounding, no mode).
inline f64 fneg(f64 b) noexcept {
    return detail::from_bits(detail::bits_of(b) ^ detail::sign_bit);
}

} // namespace gc::fp

#endif // GC_FLOAT_H
