// EpykosEngine — daily compounding of an in-arrears RFR coupon, written once and templated on
// Scalar (D3) as the NATURAL product loop (DESIGN.md §5.6, D41):
//
//   factor = Π_i (1 + r_i·τ_i)          acc = 1; for each sub-period: acc = acc · (1 + r_i·τ_i)
//   R      = (factor − 1) / τ           the compounded rate over the coupon's accrual τ
//   pv     = N·τ·R·DF(e)
//
// "Naturally" means: the loop as the definition states it, one multiplication per sub-period,
// the running product carried in a Scalar. The author never telescopes the product (with
// r_i = (DF(s_i)/DF(e_i) − 1)/τ_i it collapses to DF(s)/DF(e) on paper; that is the engine's
// business, M4), never unrolls or pre-folds, never branches on a Scalar. The signature pass then
// sees the same step acc' = acc·g_i ninety times in a row and records it as a scan domain: one
// group evaluated sequentially along the coupon and in parallel across coupons and batch lanes,
// with a reverse-scan adjoint. A sub-period whose rate is a realised fixing is written the same
// way with the fixing as a plain double (acc = acc · (1 + R_i·τ_i)): those steps form a chain of
// their own (a constant per step) whose last value is the initial value of the projected chain.
// What breaks the recurrence: writing the product as a fold over an intermediate array that is
// read elsewhere step by step (every partial product materialised is fine, it is still a scan);
// mixing two different step shapes in one loop is fine (two chains); an `if` on a Scalar is not
// (it does not compile). A chain needs at least three sub-periods.
//
// Scalar needs: construction from double, + − * / between Scalars and with double on either
// side. No comparison, no conversion to double. Nothing here branches on a value.
#pragma once

namespace epykos::ois {

// The daily forward over [s, e) from its discount factors: (DF(s)/DF(e) − 1)/τ.
template <class Scalar>
Scalar forward_rate(const Scalar& df_s, const Scalar& df_e, double tau) {
  return (df_s / df_e - 1.0) / tau;
}

// One compounding step: acc · (1 + r·τ). The same expression for a projected rate (Scalar) and a
// realised fixing (double), so that both loops below are the loop of the definition.
template <class Scalar, class Rate>
Scalar compound_step(const Scalar& acc, const Rate& r, double tau) {
  return acc * (1.0 + r * tau);
}

// Π_i (1 + r_i·τ_i) over n sub-periods, the natural loop.
template <class Scalar>
Scalar compounded_factor(const Scalar* r, const double* tau, int n) {
  Scalar acc = 1.0;
  for (int i = 0; i < n; ++i) acc = compound_step(acc, r[i], tau[i]);
  return acc;
}

// R = (factor − 1)/τ ;  pv = N·τ·R·DF(e)
template <class Scalar>
Scalar compounded_coupon_pv(double N, double tau, const Scalar& factor, const Scalar& df_e) {
  const Scalar rate = (factor - 1.0) / tau;
  return N * tau * rate * df_e;
}

}  // namespace epykos::ois
