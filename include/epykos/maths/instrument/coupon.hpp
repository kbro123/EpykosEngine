// EpykosEngine — coupon and leg maths over the row tables of tables.hpp, written once and
// templated on Scalar (D3; M3/G2). The double instantiation is the oracle, Rec records, Dual<N>
// is forward mode. Every branch is on a table field (structure); constants are the rows' doubles
// (leaves of the recording); nothing telescopes, unrolls or pre-folds by hand (that is the
// engine's business, M4): a compounded coupon is the product loop of the definition, an average
// the sum of the definition.
//
// The curve is a callable  Scalar df(int slot, double t)  giving the discount factor of curve
// `slot` at time t (Leg::curve is the projection slot, Leg::disc_curve the discount slot):
// solver::CurveStates<Scalar>::df, a Composite state per slot, a plain array — the maths never
// names a curve object.
//
//   fixed coupon         pv = N · τ · K · DF_d(t_pay)
//   float coupon         pv = N · τ · (R + s) · DF_d(t_pay), R the coupon rate below
//   compounded R         acc = realised factor; for every projected day: acc = acc · (1 + f_i · w_i),
//                        f_i = (DF_p(t_r)/DF_p(t_r') − 1) / τ_r the overnight forward FOR the day,
//                        w_i = n_i / basis its weight;  R = (acc − 1) / τ_obs
//                        (maths/swap/compounding.hpp's compound_step and forward_rate; the loop
//                        records as a scan domain when it has three or more days, D41)
//   averaged R           sum = realised Σ r_i n_i; for every projected day: sum = sum + f_i · n_i;
//                        R = sum / (calendar days of the observation period)
//   term rate R          the fixing (structure) when realised, else (DF_p(t_s)/DF_p(t_e) − 1) / τ_fix
//   accrued              structure: Coupon::accrued (the rate known so far × the accrual to today)
//
// Scalar needs: construction from double, + − * / between Scalars and with double on either
// side. No comparison, no conversion to double. Sums are left folds in row order.
#pragma once

#include <cstddef>

#include "epykos/maths/instrument/tables.hpp"
#include "epykos/maths/swap/compounding.hpp"

namespace epykos::instrument {

// The floating rate of a coupon (undefined for a fixed coupon: fixed_coupon_pv does not call it).
template <class Scalar, class Curves>
Scalar coupon_rate(const Leg& leg, const Coupon& c, Curves&& df) {
  switch (c.kind) {
    case CouponKind::RfrCompounded: {
      Scalar acc = c.realised_factor;
      for (int i = c.obs_begin; i < c.obs_end; ++i) {
        const ObsDay& d = leg.obs[static_cast<std::size_t>(i)];
        const Scalar df_r = df(leg.curve, d.t_rate);
        const Scalar df_n = df(leg.curve, d.t_next);
        const Scalar fwd = ois::forward_rate(df_r, df_n, d.tau_rate);
        acc = ois::compound_step(acc, fwd, d.weight);
      }
      return (acc - 1.0) / c.obs_tau;
    }
    case CouponKind::RfrAveraged: {
      Scalar sum = c.realised_sum;
      for (int i = c.obs_begin; i < c.obs_end; ++i) {
        const ObsDay& d = leg.obs[static_cast<std::size_t>(i)];
        const Scalar df_r = df(leg.curve, d.t_rate);
        const Scalar df_n = df(leg.curve, d.t_next);
        const Scalar fwd = ois::forward_rate(df_r, df_n, d.tau_rate);
        sum = sum + fwd * d.weight_days;
      }
      return sum / c.obs_days;
    }
    case CouponKind::TermRate: {
      if (c.realised) return Scalar(c.rate);
      const Scalar df_s = df(leg.curve, c.t_fix_start);
      const Scalar df_e = df(leg.curve, c.t_fix_end);
      return ois::forward_rate(df_s, df_e, c.fix_tau);
    }
    case CouponKind::Fixed:
      break;
  }
  return Scalar(c.rate);
}

// pv = N · τ · K · DF(t_pay)
template <class Scalar, class Curves>
Scalar fixed_coupon_pv(const Leg& leg, const Coupon& c, Curves&& df) {
  const Scalar df_pay = df(leg.disc_curve, c.t_pay);
  return c.notional * c.accrual * c.rate * df_pay;
}

// The year fraction the coupon's RATE accrues over. For an RFR coupon this is the OBSERVATION
// period's, not the calculation period's, and the two differ only under an observation shift.
//
// WHY (D122 §3, resolved against the ARRC's "An Updated User's Guide to SOFR" at the owner's
// direction). `coupon_rate` returns the annualised compounded rate (prod − 1)·basis/d_o, which is
// the ARRC's equation (B) — correctly divided by the OBSERVATION period. Appendix 4, "Methods for
// Calculating Daily Accrual in an Interest-Period Weighted Observation Shift", then asks over how
// many days equation (B) is applied, and names two answers:
//
//   (1) "apply interest for the number of days in the OBSERVATION period" — so the payment is
//       N·(prod − 1), which is what the main text means by an observation shift: "with an
//       observation shift, interest is accrued according to the number of days in the observation
//       period, which may differ from the number of days in the interest period". This is the
//       convention ISDA's protocol incorporates and the ARRC recommends for FRNs.
//   (2) apply it "for the number of days in the INTEREST period … (this variant was labeled an
//       interest-period weighted shift)" — the payment is then N·(prod − 1)·d_c/d_o.
//
// This engine computed (2) under the name `observation_shift` until 2026-10-13, and D116 measured
// the consequence at exactly d_c/d_o — 2.12 bp at 1Y against QuantLib. Option (2) is a real
// convention, "widely used in FRNs", but it is **not** what the name means: the ARRC records it as
// "not the principle recommendation", "discussed but not recommended" for business loans, and
// notes it "can at times result in a negative daily accrual even if SOFR rates are positive".
//
// So `rate_accrual` is `obs_tau` for the RFR kinds. It is a NO-OP wherever there is no shift,
// because the observation weights then sum to the calculation period's own length — plain and
// lookback coupons are bit-identical across this change, which is what makes it checkable.
inline double rate_accrual(const Coupon& c) noexcept {
  switch (c.kind) {
    case CouponKind::RfrCompounded:
    case CouponKind::RfrAveraged:
      return c.obs_tau;
    case CouponKind::TermRate:
    case CouponKind::Fixed:
      break;
  }
  return c.accrual;
}

// pv = N · τ_rate · (R + s) · DF(t_pay); with_spread = false prices the coupon at R alone (the flat
// value of a spread leg, which the par spread divides against).
//
// The SPREAD keeps the calculation period's τ: a margin is contracted on the interest period, and
// only the floating rate is observed over a shifted window. The two τ coincide unless there is an
// observation shift, and no blueprint in this repository combines a shift with a spread — the
// shifted families are OIS with spread 0 and the spread-bearing family is a EURIBOR basis whose
// legs are TermRate — so the split form below is correct but unexercised, and says so.
//
// THE SHAPE IS DELIBERATE, not incidental. `τ_rate` and `τ` are both doubles of the coupon table,
// so `tau_rate == c.accrual` is STRUCTURE and the branch provably does not depend on an input
// (recording discipline: `structural_if` only for input-independent branches). Taking it keeps the
// recorded expression for an unshifted coupon — which is every coupon of every family this project
// benchmarks — byte-identical to what it was before the ARRC fix: one multiply chain, not a sum of
// two products. The first version of this function omitted the branch and changed the recorded
// GRAPH everywhere while leaving every value identical; `adjoint_adjoint_catalogue_e0_test`,
// `exec_interpreter_catalogue_e0_test`, `rewrite_r1_fold_uniform_columns_e0_test`,
// `rewrite_r2_bucket_rows_e0_test` and `spike_telescoping_prize_test` all failed, which is them
// doing their job: a value-neutral change is not a structure-neutral one, and D81's collapse is a
// property of the structure.
template <class Scalar, class Curves>
Scalar float_coupon_pv(const Leg& leg, const Coupon& c, Curves&& df, bool with_spread = true) {
  const Scalar rate = coupon_rate<Scalar>(leg, c, df);
  const Scalar df_pay = df(leg.disc_curve, c.t_pay);
  const double tau_rate = rate_accrual(c);
  if (!with_spread) return c.notional * tau_rate * rate * df_pay;
  // `tau_rate == c.accrual` (no shift) or a zero spread: one multiply chain, the SAME ops in the
  // same order as before the ARRC fix, with only the τ constant's value differing — and for an
  // unshifted coupon not even that. Every family this project benchmarks takes this path: the
  // shifted ones are OIS with spread 0.
  if (tau_rate == c.accrual || c.spread == 0.0) return c.notional * tau_rate * (rate + c.spread) * df_pay;
  return c.notional * (rate * tau_rate + Scalar(c.spread * c.accrual)) * df_pay;
}

template <class Scalar, class Curves>
Scalar coupon_pv(const Leg& leg, const Coupon& c, Curves&& df, bool with_spread = true) {
  if (c.kind == CouponKind::Fixed) return fixed_coupon_pv<Scalar>(leg, c, df);
  return float_coupon_pv<Scalar>(leg, c, df, with_spread);
}

// Σ_j pv_j over the leg's unpaid coupons (left fold); 0 for an empty leg.
template <class Scalar, class Curves>
Scalar leg_pv(const Leg& leg, Curves&& df, bool with_spread = true) {
  if (leg.coupons.empty()) return Scalar(0.0);
  Scalar acc = coupon_pv<Scalar>(leg, leg.coupons[0], df, with_spread);
  for (std::size_t j = 1; j < leg.coupons.size(); ++j) acc = acc + coupon_pv<Scalar>(leg, leg.coupons[j], df, with_spread);
  return acc;
}

// Σ_j N_j · τ_j · DF(t_pay_j): the annuity of the leg's schedule (the par rate's denominator).
template <class Scalar, class Curves>
Scalar leg_annuity(const Leg& leg, Curves&& df) {
  if (leg.coupons.empty()) return Scalar(0.0);
  auto term = [&](std::size_t j) {
    const Coupon& c = leg.coupons[j];
    return c.notional * c.accrual * df(leg.disc_curve, c.t_pay);
  };
  Scalar acc = term(0);
  for (std::size_t j = 1; j < leg.coupons.size(); ++j) acc = acc + term(j);
  return acc;
}

// The accrued interest of the leg's current period (structure: a double of the table).
inline double leg_accrued(const Leg& leg) noexcept {
  double a = 0.0;
  for (const Coupon& c : leg.coupons) a += c.accrued;
  return a;
}

}  // namespace epykos::instrument
