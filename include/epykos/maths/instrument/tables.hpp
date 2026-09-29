// EpykosEngine — the plain row tables real instruments are priced from (M3/G2; PROBLEM.md §3).
//
// Everything in this header is STRUCTURE (D36, DESIGN.md §1): dates, day-count fractions, curve
// times, realised fixings folded into doubles, flags and index ranges — computed once at
// table-build time from the conventions registry, a fixings history and a trade (builder.hpp),
// never a Scalar. The templated maths of coupon.hpp / instrument.hpp reads these rows on
// double (the oracle), Rec (the recording) and Dual<N> (forward mode) alike, and every branch it
// takes is on a field of these tables.
//
// Curve times are years from the valuation date on ACT/365F (t = (d − valuation) / 365), the
// convention of every curve in maths/curve/ (the M1 book's t = d/365); a curve is addressed by
// a small integer SLOT (Leg::curve, Leg::disc_curve) that the caller maps to its curve objects
// (builder.hpp CurveSlots: index name -> slot, so a trade never names a curve object).
//
// Coupon kinds (the coupon mechanics of D36, code in coupon.hpp):
//   Fixed           N · τ · K · DF(t_pay)
//   RfrCompounded   compounded in arrears over the observation days of conventions/rfr.hpp
//                   (plain, lookback, observation shift, lockout): the days already fixed are the
//                   realised factor (one double), the days still to fix are projected off the
//                   curve as the natural product loop (a scan domain, D41); payment delay through
//                   t_pay; R = (Π − 1) / τ_obs on the observation period's calendar days, paid on
//                   the accrual period's day count
//   RfrAveraged     the arithmetic average of the daily rates over the observation days: the
//                   realised weighted sum plus the projected daily forwards, weighted by calendar
//                   days (it does not telescope: one forward per day)
//   TermRate        an IBOR-style rate fixed in advance: the fixing when it is in the history
//                   (structure), else the forward over the accrual period on the projection curve
//                   with the index's day count
// Every float coupon may carry a spread (the spread leg of a basis swap); accrued interest for
// the current period is structure (the rate known so far times the accrual to the valuation date).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epykos/conventions/date.hpp"
#include "epykos/conventions/daycount.hpp"

namespace epykos::instrument {

enum class CouponKind : unsigned char { Fixed = 0, RfrCompounded = 1, RfrAveraged = 2, TermRate = 3 };

const char* to_string(CouponKind k) noexcept;   // "fixed", "rfr_compounded", "rfr_averaged", "term_rate"
CouponKind coupon_kind_from_string(const std::string& s);   // throws std::invalid_argument

// A projected observation day of an RFR coupon: one entry of conventions::ObservationPeriod
// whose rate is not yet in the history. The rate FOR the day r is the overnight forward over
// the rate's own span [r, r'), r' the next fixing business day; it is applied for n_i calendar
// days in the coupon (plain: n_i = r' − r and the product telescopes; lookback / observation
// shift / lockout: the span and the weight differ, and it does not).
struct ObsDay {
  conventions::Date rate_date = 0;   // r
  double t_rate = 0.0;               // time of r
  double t_next = 0.0;               // time of r'
  double tau_rate = 0.0;             // (r' − r) / basis: the forward's accrual
  double weight = 0.0;               // n_i / basis: the step's accrual in the compounding
  double weight_days = 0.0;          // n_i: the calendar days for the arithmetic average
};

struct Coupon {
  CouponKind kind = CouponKind::Fixed;
  conventions::Date start = 0;       // accrual period [start, end)
  conventions::Date end = 0;
  conventions::Date pay = 0;         // payment date (end moved by the payment lag)
  double notional = 0.0;             // N
  double accrual = 0.0;              // τ: year fraction of [start, end) on the leg's day count
  double t_start = 0.0;
  double t_end = 0.0;
  double t_pay = 0.0;                // the discounting time
  double rate = 0.0;                 // Fixed: K. TermRate realised: the fixing. Else 0.
  double spread = 0.0;               // added to a floating rate (0 unless the leg carries the spread)
  bool realised = false;             // TermRate: the fixing is known (rate holds it)
  bool current = false;              // start < valuation < end: the period accrues today
  double accrued = 0.0;              // interest accrued from start to the valuation date (current only)
  // TermRate, projected: the forward over [t_fix_start, t_fix_end) with the index's accrual fix_tau.
  double t_fix_start = 0.0;
  double t_fix_end = 0.0;
  double fix_tau = 0.0;
  conventions::Date fixing_date = 0;
  // RFR: the realised part and the projected days leg.obs[obs_begin, obs_end).
  double realised_factor = 1.0;      // Π (1 + r_i · n_i / basis) over the fixed days
  double realised_sum = 0.0;         // Σ r_i · n_i over the fixed days (calendar-day weighted)
  int fixed_days = 0;                // Σ n_i over the fixed days
  int obs_begin = 0;
  int obs_end = 0;
  double obs_tau = 0.0;              // (obs_end − obs_start) / basis: the annualisation of the compounded rate
  double obs_days = 0.0;             // obs_end − obs_start in calendar days: the denominator of the average
  conventions::Date obs_start = 0;   // the observation period (equal to the accrual period unless shifted)
  conventions::Date obs_end_date = 0;
};

struct Leg {
  std::string index;                 // the floating index (empty on a fixed leg)
  int curve = -1;                    // projection curve slot (−1 on a fixed leg)
  int disc_curve = -1;               // discount curve slot
  bool spread_leg = false;           // the leg that carries the quoted spread
  conventions::DayCount day_count = conventions::DayCount::Act360;
  int payment_lag = 0;
  std::vector<Coupon> coupons;       // the coupons not yet paid, in accrual order
  std::vector<ObsDay> obs;           // the projected observation days of every RFR coupon
  // Reporting: the whole schedule (paid coupons included), for the tables of PROBLEM.md O2.
  std::vector<conventions::Date> schedule;   // adjusted roll dates
  std::vector<conventions::Date> payments;   // one per period
};

enum class Kind : unsigned char { Ois = 0, Irs = 1, Basis = 2, Deposit = 3, Future = 4 };

const char* to_string(Kind k) noexcept;   // "ois", "irs", "basis", "deposit", "future"

// One priced instrument: a trade of the book or a calibration instrument of a curve.
//
//   Ois / Irs   legs[0] the fixed leg, legs[1] the floating leg; side +1 receives fixed
//   Basis       legs[0] the spread leg, legs[1] the flat leg; side +1 receives the spread leg
//   Deposit     legs[0] the fixed coupon at the deposit rate, legs[1] the index forward over
//               the same period (a one-period swap); side +1 lends (receives the rate)
//   Future      legs[0] holds one coupon over the reference period (compounded, averaged or
//               fixing per the contract); price = 100 · (1 − rate); side +1 is long; the PV of a
//               position is (price − traded price) / 100 · notional, undiscounted (variation
//               margin); convexity adjustment ZERO (a stated simplification, D35)
struct Instrument {
  Kind kind = Kind::Ois;
  std::string id;                    // the trade id / the calibration key
  std::string blueprint;
  std::string convention;
  std::string currency;
  int side = +1;
  double notional = 1.0;
  double fixed_rate = 0.0;           // K (Ois / Irs / Deposit)
  double spread = 0.0;               // the spread leg's spread (Basis)
  double traded_price = 0.0;         // Future: the traded price (100 − rate · 100)
  conventions::Date trade_date = 0;
  conventions::Date effective = 0;
  conventions::Date termination = 0;
  conventions::Date last_payment = 0;
  double t_maturity = 0.0;           // the last cash-flow time (the calibration knot of this instrument)
  std::string contract_code;         // Future: "SR3 Z26", "SR1 K26", "FEU3 M26"
  std::vector<Leg> legs;
  int n_coupons() const noexcept;
  int n_obs_days() const noexcept;   // projected observation days over every leg
};

}  // namespace epykos::instrument
