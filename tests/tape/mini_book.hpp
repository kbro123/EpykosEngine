// A small Scalar-templated rates book for the tape tests and benches (M1/P2).
//
// This is P2's own fixture, not the M1 book of WORKLOADS.md (that is P1's package). It follows
// the same shape — 12-knot linear zero curve, annual ACT/360 swaps with a compounded float leg
// written uncollapsed, a seasoned bucket with a realised first fixing, a value branch — so that
// the recorder, the passes and the replay are exercised on realistic structure. Instantiated on
// `double` it is the oracle; on `Rec` it records.
//
// Header-only on purpose: the TU that includes it decides the contraction setting (E0 gates are
// *_e0_test.cpp TUs, compiled with -ffp-contract=off).
#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "epykos/scalar/rec.hpp"
#include "epykos/scalar/select.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/tape_test_helpers.hpp"

namespace epykos::test {

constexpr int kMiniKnots = 12;
constexpr std::array<double, kMiniKnots> kMiniKnotTimes = {7.0 / 365.0, 1.0 / 12.0, 0.25, 0.5, 1.0, 2.0,
                                                           3.0, 5.0, 7.0, 10.0, 20.0, 30.0};
inline const std::array<double, kMiniKnots> kMiniRecordState = {
    0.0400, 0.0405, 0.0410, 0.0415, 0.0420, 0.0410, 0.0400, 0.0395, 0.0400, 0.0410, 0.0430, 0.0440};

template <class Scalar>
struct MiniCurve {
  std::array<Scalar, kMiniKnots> z;

  // Linear in t between knots, flat beyond both ends. The bracket search is structural: t is data.
  Scalar zero(double t) const {
    if (t <= kMiniKnotTimes[0]) return z[0];
    if (t >= kMiniKnotTimes[kMiniKnots - 1]) return z[kMiniKnots - 1];
    int k = 0;
    while (kMiniKnotTimes[k + 1] < t) ++k;
    const double w = (t - kMiniKnotTimes[k]) / (kMiniKnotTimes[k + 1] - kMiniKnotTimes[k]);
    return z[k] * (1.0 - w) + z[k + 1] * w;
  }
  Scalar df(double t) const {
    if (t == 0.0) return Scalar(1.0);
    return exp(-zero(t) * t);
  }
};

struct MiniSwap {
  int tenor;          // years
  double notional;
  double side;        // +1 receive fixed, -1 pay fixed
  double fixed_rate;
  int start_day;      // <= 0
  double realised;    // first-period compounded rate when seasoned (start_day < 0)
};

template <class Scalar>
Scalar mini_swap_pv(const MiniCurve<Scalar>& curve, const MiniSwap& s) {
  using epykos::select;
  Scalar fixed = 0.0;
  Scalar floating = 0.0;
  int prev = s.start_day;
  for (int j = 1; j <= s.tenor; ++j) {
    const int end = s.start_day + static_cast<int>(std::lround(365.25 * j));
    const double tau = (end - prev) / 360.0;
    const double ts = prev / 365.0;
    const double te = end / 365.0;
    const Scalar dfe = curve.df(te);
    fixed = fixed + s.notional * tau * s.fixed_rate * dfe;
    Scalar fwd;
    if (prev < 0) {
      fwd = Scalar(s.realised);  // structural: the seasoned first coupon carries a fixing
    } else {
      const Scalar dfs = curve.df(ts);
      fwd = (dfs / dfe - 1.0) / tau;
    }
    floating = floating + s.notional * tau * fwd * dfe;
    prev = end;
  }
  const Scalar pv = s.side * (fixed - floating);
  // A value branch: floor the pv at -1e9 (nonsense, but exercises select on every swap).
  return select(pv < -1e9, Scalar(-1e9), pv);
}

inline std::vector<MiniSwap> mini_book(int n) {
  SplitMix64 rng(20260922);
  std::vector<MiniSwap> book;
  for (int i = 0; i < n; ++i) {
    MiniSwap s;
    s.tenor = 1 + static_cast<int>(rng.next() % 30);
    s.notional = std::exp(rng.uniform(std::log(1e6), std::log(1e8)));
    s.side = (rng.next() & 1) ? 1.0 : -1.0;
    s.fixed_rate = rng.uniform(0.03, 0.05);
    s.start_day = (i % 5 == 0) ? -static_cast<int>(30 + rng.next() % 271) : 0;
    s.realised = rng.uniform(0.035, 0.045);
    book.push_back(s);
  }
  return book;
}

// Evaluates the book: n swap pvs then the book pv.
template <class Scalar>
std::vector<Scalar> mini_book_pv(const MiniCurve<Scalar>& curve, const std::vector<MiniSwap>& book) {
  std::vector<Scalar> out;
  out.reserve(book.size() + 1);
  Scalar total = 0.0;
  for (const MiniSwap& s : book) {
    const Scalar pv = mini_swap_pv(curve, s);
    out.push_back(pv);
    total = total + pv;
  }
  out.push_back(total);
  return out;
}

// n states: the record point first, then perturbations of +-30 bp.
inline std::vector<std::array<double, kMiniKnots>> mini_states(int n) {
  std::vector<std::array<double, kMiniKnots>> states;
  SplitMix64 rng(100000);
  for (int b = 0; b < n; ++b) {
    std::array<double, kMiniKnots> s = kMiniRecordState;
    if (b > 0) {
      for (double& z : s) z += rng.uniform(-0.003, 0.003);
    }
    states.push_back(s);
  }
  return states;
}

// Records the book: inputs are the 12 knots, outputs the swap pvs and the book pv.
inline Tape record_mini_book(const std::vector<MiniSwap>& book) {
  Tape t;
  Tape::Scope scope(t);
  MiniCurve<Rec> curve;
  for (int k = 0; k < kMiniKnots; ++k) curve.z[k] = make_input(kMiniRecordState[k]);
  const std::vector<Rec> out = mini_book_pv(curve, book);
  for (const Rec& r : out) register_output(r);
  return t;
}

inline std::vector<double> eval_mini_book_double(const std::array<double, kMiniKnots>& state,
                                                 const std::vector<MiniSwap>& book) {
  MiniCurve<double> curve;
  curve.z = state;
  return mini_book_pv(curve, book);
}

}  // namespace epykos::test
