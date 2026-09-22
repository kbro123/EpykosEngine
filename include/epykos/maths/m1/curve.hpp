// EpykosEngine — the M1 curve (docs/WORKLOADS.md §M1): zero rate linear in t between knots, flat
// beyond both ends, DF(t) = exp(−z(t)·t).
//
// Templated on Scalar (D3). The interpolation weights depend only on t and the knot times, which
// are structure, so they are plain doubles; only the knot values z are Scalar. Nothing here
// branches on a Scalar value or converts one to double.
#pragma once

#include <cmath>

namespace epykos::m1 {

// z(t) on n knots at times knot_t[0] < ... < knot_t[n−1] with values z[0..n).
//   t <= knot_t[0]      : z[0]                     (flat extrapolation)
//   t >= knot_t[n−1]    : z[n−1]                   (flat extrapolation)
//   t == knot_t[k]      : z[k]
//   knot_t[k] < t < knot_t[k+1] : (1 − w)·z[k] + w·z[k+1],  w = (t − t_k)/(t_{k+1} − t_k)
template <class Scalar>
Scalar zero_rate(const double* knot_t, const Scalar* z, int n, double t) {
  if (t <= knot_t[0]) return z[0];
  if (t >= knot_t[n - 1]) return z[n - 1];
  int k = 0;
  while (t >= knot_t[k + 1]) ++k;  // knot_t[k] <= t < knot_t[k+1]
  if (t == knot_t[k]) return z[k];
  const double w = (t - knot_t[k]) / (knot_t[k + 1] - knot_t[k]);
  return (1.0 - w) * z[k] + w * z[k + 1];
}

// DF(t) = exp(−z(t)·t). DF(0) = exp(−z[0]·0) = 1 without a special case.
template <class Scalar>
Scalar df(const double* knot_t, const Scalar* z, int n, double t) {
  using std::exp;
  const Scalar zt = zero_rate(knot_t, z, n, t);
  return exp(-zt * t);
}

}  // namespace epykos::m1
