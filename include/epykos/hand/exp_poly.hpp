// EpykosEngine — a branch-free exp(x) on double that auto-vectorises (M1/P5).
//
// The hand-fused reference needs exp once per unique discount time and per batch lane: ~3k times
// at B = 1, ~200k values at B = 64. libm's exp is a scalar call the compiler cannot vectorise, so a
// performance engineer supplies one written as straight-line arithmetic over plain doubles: every
// loop `for (l) y[l] = exp_poly(x[l])` vectorises under -march=x86-64-v3 (or NEON) with no
// intrinsics.
//
// Method: x = k·ln2 + r with k = round(x/ln2) and |r| <= ln2/2, exp(x) = 2^k·exp(r), exp(r) by the
// degree-13 Taylor polynomial (truncation < 6e-18 relative on |r| <= 0.347), 2^k built from the
// exponent field. Every step is a single IEEE rounding (std::fma), so per-lane results are
// bit-identical between the scalar and the vectorised instantiations (no reassociation; no
// -ffast-math, D13). Error bound about 1.2 ulp worst case vs the exact value (measured against libm
// in tests/hand/exp_poly_test.cpp); the M1 gate is E1 (1e-12), so this is far inside it.
//
// Valid input range: −708 < x < 709 (k in [−1022, 1023], so 2^k is a normal double). Outside that
// the exponent-field construction wraps and the result is garbage; the M1 kernel's arguments are
// −z(t)·t in (−2, 1), and callers outside that contract must range-check themselves. NaN and
// infinities are not handled.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace epykos::hand {

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
  // exp(r) = 1 + r + r²·(c2 + r·(c3 + ... + r·c13)), Horner on the tail so the two dominant terms are
  // added last, each with one rounding.
  double s = c13;
  s = std::fma(s, r, c12);
  s = std::fma(s, r, c11);
  s = std::fma(s, r, c10);
  s = std::fma(s, r, c9);
  s = std::fma(s, r, c8);
  s = std::fma(s, r, c7);
  s = std::fma(s, r, c6);
  s = std::fma(s, r, c5);
  s = std::fma(s, r, c4);
  s = std::fma(s, r, c3);
  s = std::fma(s, r, c2);
  const double y = std::fma(r * r, s, r) + 1.0;
  // 2^k: the mantissa of t is 2^51 + k, so (bits(t) + 1023) << 52 leaves (k + 1023) in the exponent
  // field and zeros elsewhere (2^51 and the exponent of t shift out of the word).
  const std::uint64_t bits = (std::bit_cast<std::uint64_t>(t) + 1023u) << 52;
  return y * std::bit_cast<double>(bits);
}

// y[i] = exp_poly(x[i]) for i in [0, n). Written so the loop vectorises; x and y may alias
// elementwise (y == x) but must not partially overlap.
inline void exp_poly_array(const double* x, double* y, int n) noexcept {
  for (int i = 0; i < n; ++i) y[i] = exp_poly(x[i]);
}

}  // namespace epykos::hand
