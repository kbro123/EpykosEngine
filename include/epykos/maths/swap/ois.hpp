// EpykosEngine — OIS coupon formulas, written once and templated on Scalar (D3), exactly as the
// coupon formulas of docs/WORKLOADS.md §M1:
//
//   fixed coupon            pv = N·τ·K·DF(e)
//   float coupon            fwd = (DF(s)/DF(e) − 1)/τ,  pv = N·τ·fwd·DF(e)
//   seasoned first coupon   fwd replaced by the realised compounded rate R; τ still accrues s → e
//
// Telescoping the float leg is the fusion pass's job, never the author's. The leg, swap and book
// folds over a seeded fixture book are in fixtures/m1_price.hpp (test-only); M4 adds the engine's
// schedule-driven legs and swaps beside this header.
//
// Scalar needs: copy, + − * / between Scalars, and mixed arithmetic with double on either side.
// No comparison, no conversion to double, no std::max/min/abs. Nothing here branches on a value.
#pragma once

namespace epykos::ois {

// pv = N·τ·K·DF(e)
template <class Scalar>
Scalar fixed_coupon_pv(double N, double tau, double K, const Scalar& df_e) {
  return N * tau * K * df_e;
}

// fwd = (DF(s)/DF(e) − 1)/τ ;  pv = N·τ·fwd·DF(e)
template <class Scalar>
Scalar float_coupon_pv(double N, double tau, const Scalar& df_s, const Scalar& df_e) {
  const Scalar fwd = (df_s / df_e - 1.0) / tau;
  return N * tau * fwd * df_e;
}

// The seasoned first float coupon: fwd replaced by the realised rate R.  pv = N·τ·R·DF(e)
template <class Scalar>
Scalar realised_coupon_pv(double N, double tau, double R, const Scalar& df_e) {
  return N * tau * R * df_e;
}

}  // namespace epykos::ois
