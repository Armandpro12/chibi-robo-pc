// Cubic Hermite interpolation of a key-framed value (sysdolphin).
//
// Original unit: not known [hypothesis: an HSD unit placed between video.c and mtx.c, the source-file strings that
// surround the function in both versions; doldecomp/melee's sysdolphin has a spline unit with Hermite interpolation,
// not compared]. Names are hypotheses (notes of the decompilation stage); the code was written from the disassembly
// of both versions, identical in US and PAL apart from the addresses of the three constants.
#include "decomp/gc_abi.h"
#include "decomp/gc_float.h"

namespace hsd {

using gc::f32;
using gc::f64;
namespace fp = gc::fp;

// FPSCR[NI] the game runs with: 1 in every state the oracle captured, US and PAL (notes of the decompilation
// stage, D1.7) [hypothesis: the game never clears it]. To move to a header of the game when a second unit needs it.
constexpr fp::mode game_fp_mode = fp::mode::non_ieee;

// Value at `time` of the cubic Hermite curve that goes from `start_value` with slope `start_tangent` to `end_value`
// with slope `end_tangent` over a key interval whose length is 1 / `inverse_duration`. With s = time *
// inverse_duration, the normalized position in the interval:
//   start_value   * (2s^3 - 3s^2 + 1)
// + end_value     * (-2s^3 + 3s^2)
// + start_tangent * (s^3 - 2s^2 + s) / inverse_duration
// + end_tangent   * (s^3 - s^2) / inverse_duration
// written, as in the original, with powers of time and inverse_duration (no division). Every step is the
// single-precision instruction of the original, in its order and with its operand order (gc_float.h: fmuls rounds
// its second operand to 25 bits, fmadds is fused); the arguments are the full registers f1..f6 (f64).
f32 hermite_interpolate(f64 inverse_duration, f64 time, f64 start_value, f64 end_value, f64 start_tangent,
                        f64 end_tangent) {
    constexpr f64 two = 2.0, three = 3.0, one = 1.0; // single-precision constants of the original
    const f64 a = inverse_duration;
    const f64 t2 = fp::fmuls(game_fp_mode, time, time);
    const f64 a2 = fp::fmuls(game_fp_mode, a, a);
    const f64 t3 = fp::fmuls(game_fp_mode, t2, time);
    const f64 three_t2 = fp::fmuls(game_fp_mode, three, t2);
    const f64 a_t2 = fp::fmuls(game_fp_mode, t2, a);
    const f64 a2_t3 = fp::fmuls(game_fp_mode, a2, t3);
    const f64 three_a2_t2 = fp::fmuls(game_fp_mode, three_t2, a2);
    const f64 two_a2_t3 = fp::fmuls(game_fp_mode, two, a2_t3);
    const f64 end_tangent_weight = fp::fsubs(game_fp_mode, a2_t3, a_t2);         // (s^3 - s^2) / a
    const f64 two_a3_t3 = fp::fmuls(game_fp_mode, two_a2_t3, a);
    const f64 cubic_minus_two_square = fp::fsubs(game_fp_mode, end_tangent_weight, a_t2); // (s^3 - 2s^2) / a
    const f64 minus_two_a3_t3 = fp::fneg(two_a3_t3);
    const f64 two_s3_minus_three_s2 = fp::fsubs(game_fp_mode, two_a3_t3, three_a2_t2);
    const f64 start_tangent_weight = fp::fadds(game_fp_mode, time, cubic_minus_two_square); // (s^3 - 2s^2 + s) / a
    const f64 end_value_weight = fp::fadds(game_fp_mode, minus_two_a3_t3, three_a2_t2);    // -2s^3 + 3s^2
    const f64 start_value_weight = fp::fadds(game_fp_mode, one, two_s3_minus_three_s2);    // 2s^3 - 3s^2 + 1
    f64 sum = fp::fmuls(game_fp_mode, end_value, end_value_weight);
    sum = fp::fmadds(game_fp_mode, start_value, start_value_weight, sum);
    sum = fp::fmadds(game_fp_mode, start_tangent, start_tangent_weight, sum);
    return fp::fmadds(game_fp_mode, end_tangent, end_tangent_weight, sum);
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_hermite_interpolate, hsd::hermite_interpolate, gc::abi::flag_reads_only);
