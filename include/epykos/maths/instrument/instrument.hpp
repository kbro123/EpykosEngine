// EpykosEngine — instrument maths over the row tables (M3/G2): the PV of a trade for the book
// (PROBLEM.md O2), its par rate / par spread / implied rate for the quote, and the residual a
// calibration drives to zero (solver/curve_set.hpp, D40) — written once and templated on Scalar
// (D3). The curve is the callable  Scalar df(int slot, double t)  of coupon.hpp.
//
//   Ois / Irs   pv  = side · (Σ fixed − Σ float)
//               par = Σ float / Σ N τ DF  (the fixed leg's annuity)        residual = par − q
//   Basis       pv  = side · (Σ spread leg − Σ flat leg)
//               par spread = (Σ flat leg − Σ spread leg at R alone) / annuity(spread leg)
//                                                                          residual = par − q
//   Deposit     pv  = side · N τ (K − f) DF(t_e)   (legs[0] the fixed coupon at K, legs[1] the
//               index forward over the same period);  par = f = (DF_p(t_s)/DF_p(t_e) − 1)/τ
//                                                                          residual = f − q
//   Future      rate = the reference-period coupon's rate; price = 100 (1 − rate);
//               pv = side · N · (price − traded price) / 100, undiscounted;
//               residual = rate − (1 − q/100)  (rate units, like every other residual, so the
//               solver's ‖F‖∞ tolerance means the same thing on every instrument);
//               convexity adjustment ZERO, a stated simplification (D35, PROBLEM.md §9)
//
// Sides: +1 receives legs[0] (fixed / spread leg / the deposit rate) or is long the future.
// Nothing here branches on a value: the kind and the side are table fields.
#pragma once

#include <stdexcept>
#include <string>

#include "epykos/maths/instrument/coupon.hpp"
#include "epykos/maths/instrument/tables.hpp"

namespace epykos::instrument {

// price = 100 · (1 − rate): the quotation of every money-market future here.
template <class Scalar>
Scalar futures_price(const Scalar& rate) {
  return 100.0 * (1.0 - rate);
}
// rate = 1 − price / 100: the inverse, applied to a quote.
template <class Scalar>
Scalar futures_rate(const Scalar& price) {
  return 1.0 - price / 100.0;
}

namespace detail {
inline void require_legs(const Instrument& in, std::size_t n, const char* what) {
  if (in.legs.size() != n) {
    throw std::logic_error(std::string("instrument::") + what + ": '" + in.id + "' (" + to_string(in.kind) + ") needs " +
                           std::to_string(n) + " legs, has " + std::to_string(in.legs.size()));
  }
}
inline const Coupon& futures_coupon(const Instrument& in, const char* what) {
  require_legs(in, 1, what);
  if (in.legs[0].coupons.size() != 1) {
    throw std::logic_error(std::string("instrument::") + what + ": future '" + in.id + "' needs exactly one reference-period coupon");
  }
  return in.legs[0].coupons[0];
}
}  // namespace detail

// The floating rate a future settles to (its reference-period coupon's rate).
template <class Scalar, class Curves>
Scalar futures_settlement_rate(const Instrument& in, Curves&& df) {
  const Coupon& c = detail::futures_coupon(in, "futures_settlement_rate");
  return coupon_rate<Scalar>(in.legs[0], c, df);
}

// The PV of the trade in its currency (the book's O2).
template <class Scalar, class Curves>
Scalar pv(const Instrument& in, Curves&& df) {
  const double side = static_cast<double>(in.side);
  switch (in.kind) {
    case Kind::Ois:
    case Kind::Irs:
    case Kind::Basis:
    case Kind::Deposit: {
      detail::require_legs(in, 2, "pv");
      const Scalar receive = leg_pv<Scalar>(in.legs[0], df);
      const Scalar pay = leg_pv<Scalar>(in.legs[1], df);
      return side * (receive - pay);
    }
    case Kind::Future: {
      const Scalar rate = futures_settlement_rate<Scalar>(in, df);
      const Scalar price = futures_price(rate);
      return side * in.notional * (price - in.traded_price) / 100.0;
    }
  }
  throw std::logic_error("instrument::pv: bad kind");
}

// The par quote of the instrument on the curves: par rate (Ois / Irs), par spread (Basis), the
// index forward (Deposit), the futures price (Future).
template <class Scalar, class Curves>
Scalar par(const Instrument& in, Curves&& df) {
  switch (in.kind) {
    case Kind::Ois:
    case Kind::Irs: {
      detail::require_legs(in, 2, "par");
      return leg_pv<Scalar>(in.legs[1], df) / leg_annuity<Scalar>(in.legs[0], df);
    }
    case Kind::Basis: {
      detail::require_legs(in, 2, "par");
      const Scalar flat = leg_pv<Scalar>(in.legs[1], df);
      const Scalar spread_leg_flat = leg_pv<Scalar>(in.legs[0], df, /*with_spread=*/false);
      return (flat - spread_leg_flat) / leg_annuity<Scalar>(in.legs[0], df);
    }
    case Kind::Deposit: {
      detail::require_legs(in, 2, "par");
      if (in.legs[1].coupons.size() != 1) throw std::logic_error("instrument::par: deposit '" + in.id + "' needs one index coupon");
      return coupon_rate<Scalar>(in.legs[1], in.legs[1].coupons[0], df);
    }
    case Kind::Future:
      return futures_price(futures_settlement_rate<Scalar>(in, df));
  }
  throw std::logic_error("instrument::par: bad kind");
}

// The calibration residual for the quote q: par − q in rate units (a future's quote is a price:
// rate(curves) − (1 − q/100)).
template <class Scalar, class Curves>
Scalar residual(const Instrument& in, Curves&& df, const Scalar& quote) {
  if (in.kind == Kind::Future) return futures_settlement_rate<Scalar>(in, df) - futures_rate(quote);
  return par<Scalar>(in, df) - quote;
}

}  // namespace epykos::instrument
