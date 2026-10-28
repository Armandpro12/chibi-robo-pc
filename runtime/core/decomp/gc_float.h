// Single-precision arithmetic of the GameCube CPU (Gekko) for decompiled code (generic GameCube runtime; no reference
// to any specific game).
//
// Why: a PowerPC single-precision instruction does not compute what the same C++ expression on `float` computes on a
// PC in every case, and the decompiled code must give the recompiled original's results bit for bit (the oracle
// compares them). Each function below is one instruction, with the behavior of the recompiler's helpers that the
// recompiled code calls (DolRecomp src/cpu/cpu.c: ppc_fadds, ppc_fsubs, ppc_fmuls, ppc_fma; GPL-3.0, behavior only,
// no code taken), cross-checked on the points below against Dolphin's interpreter (Source/Core/Core/PowerPC/
// Interpreter/Interpreter_FPUtils.h, Force25Bit; notes of the decompilation stage, section D1.3):
//   - every operation is computed in double precision, then rounded once to single (fadds, fsubs, fmuls), which
//     `static_cast<f32>(double)` does on the host in round-to-nearest;
//   - fmuls and the fmadds family first round their frC operand to a 25-bit significand (Force25Bit): `float` * `float`
//     on the host gives other results;
//   - fmadds computes a*c + b with one rounding to double (std::fma), then to single; a result that falls exactly half
//     way between two singles after the first rounding is moved one double step toward the exact value first, so that
//     the second rounding goes the way a single rounding of the exact value would (DolRecomp does the same);
//   - NaN results: the first NaN operand, made quiet, in the order a, b (then c for fmadds); otherwise the default
//     NaN 0x7FF8000000000000 (an invalid operation such as inf - inf), not the host's negative default NaN.
// Limits (the conditions under which these functions equal the instructions):
//   - FPSCR as the decompiled code finds it: round to nearest (RN = 0), non-IEEE mode off (NI = 0), no exception
//     enabled. A game that changes them needs other helpers [each game checks it: e.g. no mtfsf/mtfsb in its code];
//     FPSCR's status bits (FPRF, FI, FR, sticky exceptions) are not computed, the oracle does not compare FPSCR;
//   - operands are the full double-precision register values (pass f64, as the instruction reads them); the result
//     of fmadds on a NaN operand that is not a single-precision value keeps the double NaN in the register, which an
//     f32 result cannot hold [hypothesis: does not happen with operands that come from single-precision loads or
//     operations, whose NaNs are singles].
// Use: f32 x = gc::fp::fmuls(a, c); the order of the operands is the instruction's (frA, frC, frB), not commutative.
#ifndef GC_FLOAT_H
#define GC_FLOAT_H

#include <bit>
#include <cmath>
#include <cstdint>

namespace gc::fp {

using f32 = float;
using f64 = double;

namespace detail {

inline constexpr std::uint64_t sign_bit = 0x8000000000000000ull;
inline constexpr std::uint64_t exponent_bits = 0x7FF0000000000000ull;
inline constexpr std::uint64_t fraction_bits = 0x000FFFFFFFFFFFFFull;
inline constexpr std::uint64_t quiet_bit = 0x0008000000000000ull;
inline constexpr std::uint64_t default_nan = 0x7FF8000000000000ull;

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

} // namespace detail

// frC as fmuls and the fmadds family use it: the significand rounded to 25 bits, half up. For a normal operand the
// bits of the fraction below bit 27 are dropped and bit 27 is added to itself (a set bit 27 carries into bit 28, which
// may reach the exponent; the result always has bit 27 clear). For a subnormal operand the window moves down with
// its leading one; one with a leading one at fraction bit 23 or lower is kept as it is.
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
inline f32 fadds(f64 a, f64 b) noexcept {
    const f64 r = a + b;
    return static_cast<f32>(std::isnan(r) ? detail::nan_result(a, b) : r);
}

// fsubs frD, frA, frB
inline f32 fsubs(f64 a, f64 b) noexcept {
    const f64 r = a - b;
    return static_cast<f32>(std::isnan(r) ? detail::nan_result(a, b) : r);
}

// fmuls frD, frA, frC
inline f32 fmuls(f64 a, f64 c) noexcept {
    const f64 c25 = round_multiplier_25(c);
    const f64 r = a * c25;
    return static_cast<f32>(std::isnan(r) ? detail::nan_result(a, c25) : r);
}

// fmadds frD, frA, frC, frB: a * c + b.
inline f32 fmadds(f64 a, f64 c, f64 b) noexcept {
    const f64 c25 = round_multiplier_25(c);
    f64 r = std::fma(a, c25, b);
    std::uint64_t rb = detail::bits_of(r);
    // Half way between two singles: the 29 fraction bits a single does not hold are exactly 1 followed by zeros.
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

// fneg frD, frB: the sign bit flipped, nothing else (also for a NaN; no rounding).
inline f64 fneg(f64 b) noexcept {
    return detail::from_bits(detail::bits_of(b) ^ detail::sign_bit);
}

} // namespace gc::fp

#endif // GC_FLOAT_H
