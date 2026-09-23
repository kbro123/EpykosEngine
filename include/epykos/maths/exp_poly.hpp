// EpykosEngine — a branch-free exp(x) on double that auto-vectorises (M1/P5; engine maths since
// D28: exec::Interpreter's ExpMode::poly and the hand-fused reference in bench/hand/ both use it).
//
// A rates book needs exp once per unique discount time and per batch lane: ~3k times at B = 1,
// ~200k values at B = 64. libm's exp is a scalar call the compiler cannot vectorise, so a
// performance engineer supplies one written as straight-line arithmetic over plain doubles: every
// loop `for (l) y[l] = exp_poly(x[l])` vectorises under -march=x86-64-v3 (or NEON) with no
// intrinsics.
//
// Method: x = k·ln2 + r with k = round(x/ln2) and |r| <= ln2/2, exp(x) = 2^k·exp(r), exp(r) by the
// degree-13 Taylor polynomial (truncation < 6e-18 relative on |r| <= 0.347) evaluated in Estrin
// form so the dependent chain is ~12 operations rather than a 17-deep Horner chain (the fma
// latency, not the throughput, bounds a loop of independent exps), 2^k built from the exponent
// field. Every step is a single IEEE rounding (std::fma), so per-lane results are bit-identical
// between the scalar and the vectorised instantiations (no reassociation; no -ffast-math, D13).
// Error bound about 1.2 ulp worst case vs the exact value (measured against libm in
// tests/maths/exp_poly_test.cpp); the M1 gate is E1 (1e-12), so this is far inside it.
//
// Valid input range: −708 < x < 709 (k in [−1022, 1023], so 2^k is a normal double). Outside that
// the exponent-field construction wraps and the result is garbage; the M1 kernel's arguments are
// −z(t)·t in (−2, 1), and callers outside that contract must range-check themselves. NaN and
// infinities are not handled.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace epykos::maths {

namespace exp_poly_detail {

inline constexpr double log2e = 1.4426950408889634074;    // 1/ln2
inline constexpr double shift = 6755399441055744.0;        // 1.5·2^52: fma(x, log2e, shift) − shift = round(x/ln2)
inline constexpr double ln2_hi = 6.93147180559945286227e-01;  // nearest double to ln2
inline constexpr double ln2_lo = 2.31904681384629955842e-17;  // ln2 − ln2_hi to double precision

// 1/n! for n = 2..13, correctly rounded by constexpr division.
inline constexpr double c2 = 1.0 / 2.0;
inline constexpr double c3 = 1.0 / 6.0;
inline constexpr double c4 = 1.0 / 24.0;
inline constexpr double c5 = 1.0 / 120.0;
inline constexpr double c6 = 1.0 / 720.0;
inline constexpr double c7 = 1.0 / 5040.0;
inline constexpr double c8 = 1.0 / 40320.0;
inline constexpr double c9 = 1.0 / 362880.0;
inline constexpr double c10 = 1.0 / 3628800.0;
inline constexpr double c11 = 1.0 / 39916800.0;
inline constexpr double c12 = 1.0 / 479001600.0;
inline constexpr double c13 = 1.0 / 6227020800.0;

}  // namespace exp_poly_detail

inline double exp_poly(double x) noexcept {
  using namespace exp_poly_detail;
  // k = round(x/ln2) as a double, and its two's-complement image in the low mantissa bits of t.
  const double t = std::fma(x, log2e, shift);
  const double k = t - shift;
  // r = x − k·ln2 in two fused steps: |r| <= ln2/2 with about 0.3 ulp(r) absolute error.
  double r = std::fma(-k, ln2_hi, x);
  r = std::fma(-k, ln2_lo, r);
  // exp(r) = 1 + r + r²·s(r), s(r) = c2 + c3·r + ... + c13·r^11 by Estrin: six pairs in r, three
  // quads in r², then r⁴ and r⁸; the two dominant terms are added last, each with one rounding.
  const double r2 = r * r;
  const double r4 = r2 * r2;
  const double r8 = r4 * r4;
  const double p01 = std::fma(c3, r, c2);
  const double p23 = std::fma(c5, r, c4);
  const double p45 = std::fma(c7, r, c6);
  const double p67 = std::fma(c9, r, c8);
  const double p89 = std::fma(c11, r, c10);
  const double pab = std::fma(c13, r, c12);
  const double q0 = std::fma(p23, r2, p01);
  const double q1 = std::fma(p67, r2, p45);
  const double q2 = std::fma(pab, r2, p89);
  const double s = std::fma(q2, r8, std::fma(q1, r4, q0));
  const double y = std::fma(r2, s, r) + 1.0;
  // 2^k: the mantissa of t is 2^51 + k, so (bits(t) + 1023) << 52 leaves (k + 1023) in the exponent
  // field and zeros elsewhere (2^51 and the exponent of t shift out of the word).
  const std::uint64_t bits = (std::bit_cast<std::uint64_t>(t) + 1023u) << 52;
  return y * std::bit_cast<double>(bits);
}

// y[i] = exp_poly(x[i]) for i in [0, n). Written so the loop vectorises, with several vectors in
// flight per iteration so the dependent chain of one exp overlaps the others; x and y may alias
// elementwise (y == x) but must not partially overlap.
inline void exp_poly_array(const double* x, double* y, int n) noexcept {
#if defined(__clang__)
#pragma clang loop vectorize(enable) interleave_count(4)
#elif defined(__GNUC__)
#pragma GCC unroll 4
#endif
  for (int i = 0; i < n; ++i) y[i] = exp_poly(x[i]);
}

}  // namespace epykos::maths
