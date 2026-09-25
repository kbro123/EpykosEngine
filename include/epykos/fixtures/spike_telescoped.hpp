// EpykosEngine — SPIKE, MEASUREMENT ONLY: the compounded OIS coupon written a SECOND time, by
// hand, in its telescoped form, so that the prize of finding that rewrite automatically can be
// measured before the machinery to find it is built.
//
// ============================== READ THIS BEFORE MOVING ANY OF IT ==============================
//
// **This is not production maths and must never become production maths.** `docs/PRINCIPLES.md`
// §2 ("Discovery, not declaration") is explicit that the pricing maths declares no optimisation
// opportunity: "a compounded coupon is the product loop of the definition". Hand-telescoping the
// coupon in `include/epykos/maths/instrument/coupon.hpp` would violate that outright, and that
// header says so itself — "nothing telescopes, unrolls or pre-folds by hand (that is the engine's
// business)". Nothing here is included by any engine header (D28); everything lives under
// `fixtures/` and is named `spike`.
//
// **What it exists for.** PRINCIPLES.md §8 orders the rebuild and puts telescoping fourth, "as the
// worked example — it exercises every part of this contract and is worth 250:1". That 250:1 comes
// from `bench/compare/README.md` §4 item 1 and D71 §6(a), and it is a count of RECORDED nodes: the
// 49,091 projected observation days of the sixteen `compare_ois` calibration instruments against
// the 197 telescoped coupon-periods the other engine is given. Nobody has measured what survives
// of that ratio after common subexpression elimination, the affine collapse, domain inference and
// planning, and the README warns against reading a recorded count as a cost. This header records
// both forms from ONE fixture so the difference can be measured end to end instead of inferred.
//
// **The engine is expected to find this itself.** The identity below is a general fact about a
// scan whose step multiplies by a ratio of consecutive terms, which PRINCIPLES.md §2 names as
// exactly the kind of fact the engine is entitled to hold. When the recurrence rule kind of §2a
// exists, this header becomes redundant and should be deleted rather than kept as a fast path.
//
// ---------------------------------------------------------------------------------------------
//
// THE IDENTITY, and the precondition it needs. For the plain observation method the projected
// observation days tile the observation period — day i's forward spans [t_rate_i, t_next_i) and
// t_next_i is t_rate_{i+1} — and each day's compounding weight w_i is that same span's own
// accrual tau_rate_i, because both are the same calendar-day count over the same basis. Then
//
//     1 + f_i·w_i  =  1 + ((DF(t_rate_i)/DF(t_next_i) − 1)/tau_rate_i)·w_i  =  DF(t_rate_i)/DF(t_next_i)
//
// and the product over the coupon's days telescopes:
//
//     Π_i (1 + f_i·w_i)  =  DF(t_rate_first) / DF(t_next_last).
//
// So `acc` — the whole scan of `maths/swap/compounding.hpp`, one multiply per projected day —
// collapses to one division of two discount factors the curve is already being asked for.
//
// It is an identity in ℝ, i.e. PRINCIPLES.md §1 tier 1, exact algebra. It is NOT an identity in
// floating point: the two forms are different sequences of roundings. Which one is closer to
// truth is measured, not assumed (`tests/spike/telescoping_prize_test.cpp` measures both against
// `epykos::Wide`, the D72 oracle, and PRINCIPLES.md §4 is what licenses using it as a diagnostic).
//
// `telescopes()` CHECKS the precondition rather than assuming it, per coupon, on the tables; a
// coupon that fails it (lookback, observation shift, lockout, a capped final day, an averaged or
// term coupon) falls back to the naive form, so a telescoped recording of a book that does not
// telescope is the naive recording and the measurement reports zero prize rather than a wrong
// number.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/maths/instrument/calibrate.hpp"
#include "epykos/maths/instrument/instrument.hpp"
#include "epykos/maths/instrument/tables.hpp"
#include "epykos/solver/curve_set.hpp"

namespace epykos::fixtures::spike {

// ---- the precondition -------------------------------------------------------------------------

// True when coupon `c` of `leg` is a compounded RFR coupon whose projected observation days
// satisfy the telescoping precondition exactly: at least one projected day, every day's
// compounding weight bitwise equal to its own forward accrual, and consecutive days meeting
// (t_next_i == t_rate_{i+1}). Exact comparison is the right test: both quantities are the same
// integer calendar-day count divided by the same basis, so they are bitwise equal or the coupon
// is not a plain observation and does not telescope at all.
bool telescopes(const instrument::Leg& leg, const instrument::Coupon& c) noexcept;

// Every compounded coupon of every leg of `in` telescopes.
bool telescopes(const instrument::Instrument& in) noexcept;

// ---- the coupon maths, written the second time --------------------------------------------------
//
// A duplicate of the chain in maths/instrument/coupon.hpp and maths/instrument/instrument.hpp,
// differing in exactly one place: `spike_coupon_rate`'s RfrCompounded arm. Everything else is the
// same statement so that the measurement compares the two coupon forms and nothing else.

template <class Scalar, class Curves>
Scalar spike_coupon_rate(const instrument::Leg& leg, const instrument::Coupon& c, Curves&& df) {
  using namespace instrument;
  if (c.kind == CouponKind::RfrCompounded && telescopes(leg, c)) {
    // THE ONE LINE THIS WHOLE HEADER EXISTS FOR. acc = realised · Π_i DF(t_rate_i)/DF(t_next_i),
    // and the product is the ratio of the period's endpoint discount factors.
    const ObsDay& first = leg.obs[static_cast<std::size_t>(c.obs_begin)];
    const ObsDay& last = leg.obs[static_cast<std::size_t>(c.obs_end - 1)];
    const Scalar df_first = df(leg.curve, first.t_rate);
    const Scalar df_last = df(leg.curve, last.t_next);
    const Scalar acc = c.realised_factor * (df_first / df_last);
    return (acc - 1.0) / c.obs_tau;
  }
  return coupon_rate<Scalar>(leg, c, df);   // every other kind is the engine's own maths verbatim
}

template <class Scalar, class Curves>
Scalar spike_coupon_pv(const instrument::Leg& leg, const instrument::Coupon& c, Curves&& df, bool with_spread = true) {
  using namespace instrument;
  if (c.kind == CouponKind::Fixed) return fixed_coupon_pv<Scalar>(leg, c, df);
  const Scalar rate = spike_coupon_rate<Scalar>(leg, c, df);
  const Scalar df_pay = df(leg.disc_curve, c.t_pay);
  if (with_spread) return c.notional * c.accrual * (rate + c.spread) * df_pay;
  return c.notional * c.accrual * rate * df_pay;
}

template <class Scalar, class Curves>
Scalar spike_leg_pv(const instrument::Leg& leg, Curves&& df, bool with_spread = true) {
  if (leg.coupons.empty()) return Scalar(0.0);
  Scalar acc = spike_coupon_pv<Scalar>(leg, leg.coupons[0], df, with_spread);
  for (std::size_t j = 1; j < leg.coupons.size(); ++j) {
    acc = acc + spike_coupon_pv<Scalar>(leg, leg.coupons[j], df, with_spread);
  }
  return acc;
}

// The PV of an instrument. Only the kinds this fixture uses (Ois / Irs / Basis / Deposit) are
// written out; a Future would need `futures_settlement_rate` and the `compare_ois` fixture has
// none, so it throws rather than silently pricing on the naive path.
template <class Scalar, class Curves>
Scalar spike_pv(const instrument::Instrument& in, Curves&& df) {
  using namespace instrument;
  if (in.kind == Kind::Future) throw std::logic_error("spike_pv: the telescoping spike does not price futures");
  instrument::detail::require_legs(in, 2, "spike_pv");
  const double side = static_cast<double>(in.side);
  const Scalar receive = spike_leg_pv<Scalar>(in.legs[0], df);
  const Scalar pay = spike_leg_pv<Scalar>(in.legs[1], df);
  return side * (receive - pay);
}

template <class Scalar, class Curves>
Scalar spike_par(const instrument::Instrument& in, Curves&& df) {
  using namespace instrument;
  if (in.kind != Kind::Ois && in.kind != Kind::Irs) {
    throw std::logic_error("spike_par: the telescoping spike calibrates OIS / IRS only");
  }
  instrument::detail::require_legs(in, 2, "spike_par");
  return spike_leg_pv<Scalar>(in.legs[1], df) / leg_annuity<Scalar>(in.legs[0], df);
}

template <class Scalar, class Curves>
Scalar spike_residual(const instrument::Instrument& in, Curves&& df, const Scalar& quote) {
  return spike_par<Scalar>(in, df) - quote;
}

// ---- the two recordings -------------------------------------------------------------------------

enum class Form : int {
  naive = 0,        // the engine's own maths: maths/instrument/coupon.hpp, the product loop
  telescoped = 1,   // the same problem with `spike_coupon_rate` above
};

const char* to_string(Form f) noexcept;

// `instrument::add_calibration_set` with the residual replaced by `spike_residual`. Everything
// else — the CurveSpec, the knots, the regions, the start values, the instrument order — is the
// same, so the two CurveSets differ only in the coupon form.
int add_calibration_set_spike(solver::CurveSet& set, const instrument::CalibrationSet& cs, std::span<const double> start,
                              Form form);

// `fixtures::record_compare_ois` with the coupon form selectable. `form == Form::naive` records
// the SAME tape as `record_compare_ois` does — `tests/spike/telescoping_prize_test.cpp` asserts
// the node counts agree, which is what makes the telescoped column a measurement of the coupon
// form and not of two different harnesses.
CompareOisTape record_compare_ois_spike(const CompareOis& s, Form form, bool passes = true);

// ---- the structural counts the spike reports ------------------------------------------------------

struct FormCounts {
  Form form = Form::naive;
  int trades = 0;
  int quotes = 0;
  int obs_days_calibration = 0;   // projected observation days recorded on the calibration side
  int obs_days_book = 0;
  int coupons_calibration = 0;    // telescoped coupon-periods: what the other engine is given
  int coupons_book = 0;
  std::size_t tape_nodes_raw = 0;
  std::size_t tape_nodes_after_passes = 0;
  // ir::infer of the whole program (the residual sub-programs and the book in one IR), as
  // solver::ImplicitProgram builds it.
  std::size_t ir_values = 0;
  std::size_t ir_domains = 0;
  std::size_t ir_scan_domains = 0;
  std::size_t ir_scan_rows = 0;     // rows of every scan domain: the steps actually iterated
  std::size_t ir_literals = 0;
  std::size_t ir_columns = 0;
  std::size_t ir_column_rows = 0;
  std::size_t ir_gathers = 0;
  std::size_t ir_gather_rows = 0;
  std::size_t ir_segments = 0;
  std::size_t ir_segment_members = 0;
  std::size_t ir_steps = 0;         // group steps summed over domains, weighted by rows: the work
  // The calibration block's own residual slice, which is what the Newton solve iterates and what
  // the calibrate row's wall clock is made of. The whole-program IR above is what `evaluate` runs.
  std::size_t residual_values = 0;
  std::size_t residual_domains = 0;
  std::size_t residual_steps = 0;
  std::size_t residual_scan_rows = 0;
  double seconds_record = 0.0;
  std::string to_string() const;
};

// Records `s` in `form`, builds the ImplicitProgram, and fills in every count above.
FormCounts count_compare_ois_spike(const CompareOis& s, Form form);

// A two-column report of both forms with the ratios, for the spike's own output.
std::string compare_forms(const FormCounts& naive, const FormCounts& telescoped);

}  // namespace epykos::fixtures::spike
