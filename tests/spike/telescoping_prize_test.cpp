// SPIKE, MEASUREMENT ONLY — what telescoping the compounded coupon is worth, structurally and in
// accuracy, measured on the `compare_ois` fixture (D74).
//
// Read `include/epykos/fixtures/spike_telescoped.hpp` first. The telescoped coupon is hand-written
// TEST-ONLY code that must never become production maths: PRINCIPLES.md §2 says the pricing maths
// declares no optimisation opportunity, and the engine is expected to find this rewrite itself
// through the recurrence rule kind of §2a. This file exists to price that rewrite BEFORE the
// machinery to find it is built, because `bench/compare/README.md` §4 item 1's "about 250:1" is a
// count of RECORDED nodes and that README warns in the same paragraph against reading a recorded
// count as a cost.
//
// What is gated here, and what is only reported:
//
//   GATED   the telescoping precondition holds on every compounded coupon of the fixture;
//           the naive column of this spike's own harness reproduces `record_compare_ois` exactly,
//           so the telescoped column measures the coupon form and not two harnesses;
//           the two forms agree on O1, O2 and O3 within a loose tolerance (PRINCIPLES.md §4: the
//           operative contract is verified identities and path-to-path agreement, not bit
//           identity);
//           the telescoped form is not LESS accurate than the naive one against the D72 oracle.
//   REPORTED (printed, never asserted): the node, domain and IR counts of both forms, and the
//           measured error of each against `epykos::Wide`. Wall clock is not measured here at all
//           — it is `bench/spike/telescoping_bench.cpp` under bench/run.sh's load rule (D9).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/spike_telescoped.hpp"
#include "epykos/maths/instrument/instrument.hpp"
#include "epykos/scalar/wide.hpp"
#include "epykos/solver/implicit_program.hpp"

using namespace epykos;
namespace spike = epykos::fixtures::spike;

namespace {

// The gate instance: six calibration instruments on six knots and eight book trades (D64), the
// same shape `tests/compare/ois_test.cpp` gates on. The shape is what is being gated; the size
// that matters for the measurement is the sixteen-instrument one below.
fixtures::CompareOisOptions small_options() {
  fixtures::CompareOisOptions o;
  o.tenors = {"1Y", "2Y", "3Y", "5Y", "7Y", "10Y"};
  o.trades = 8;
  return o;
}

// The measurement instance: the full sixteen calibration instruments of
// `compare_ois_default_tenors()` — one year through ten years annually, then twelve, fifteen,
// twenty, twenty-five, thirty and forty years — against a sixteen-trade book (D64). This is the
// instance D71 §6(a) counted 49,091 projected observation days on.
fixtures::CompareOisOptions full_options() {
  fixtures::CompareOisOptions o;
  o.trades = 16;
  return o;
}

double rel(double a, double b, double scale) { return scale == 0.0 ? std::fabs(a - b) : std::fabs(a - b) / scale; }

// A discount-factor memo over an arbitrary Scalar, so the oracle evaluation does the same amount
// of curve work the recording does.
template <class Scalar, class Inner>
class Memo {
 public:
  explicit Memo(Inner inner) : inner_(std::move(inner)) {}
  Scalar operator()(int slot, double t) {
    std::uint64_t key = 0;
    std::memcpy(&key, &t, sizeof key);
    auto it = table_.find(key);
    if (it != table_.end()) return it->second;
    const Scalar v = inner_(slot, t);
    table_.emplace(key, v);
    return v;
  }

 private:
  Inner inner_;
  std::unordered_map<std::uint64_t, Scalar> table_;
};

// The book PV and every calibration instrument's par rate, on a FIXED knot vector, in `Scalar`,
// in the given coupon form. No solve: the two forms are handed the same curve so the comparison
// is of the coupon arithmetic and of nothing else.
template <class Scalar>
struct Priced {
  Scalar book = Scalar(0.0);
  std::vector<Scalar> pv;
  std::vector<Scalar> par;
};

template <class Scalar>
Priced<Scalar> price_at(const fixtures::CompareOis& s, const std::vector<double>& z, spike::Form form) {
  const curve::Composite comp = s.set.composite();
  std::vector<Scalar> zs;
  zs.reserve(z.size());
  for (double v : z) zs.push_back(Scalar(v));
  const auto st = comp.prepare<Scalar>(zs.data());
  Memo<Scalar, std::function<Scalar(int, double)>> memo([&comp, &st](int, double t) { return comp.df(st, t); });
  Priced<Scalar> r;
  for (const instrument::CalibrationInstrument& ci : s.set.instruments) {
    r.par.push_back(form == spike::Form::telescoped ? spike::spike_par<Scalar>(ci.instrument, memo)
                                                    : instrument::par<Scalar>(ci.instrument, memo));
  }
  Scalar total = Scalar(0.0);
  for (const instrument::Instrument& in : s.book) {
    const Scalar p = form == spike::Form::telescoped ? spike::spike_pv<Scalar>(in, memo) : instrument::pv<Scalar>(in, memo);
    r.pv.push_back(p);
    total = total + p;
  }
  r.book = total;
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// 1. The precondition. If this fails, everything below is measuring a fallback to the naive form.
// ---------------------------------------------------------------------------------------------
TEST(SpikeTelescoping, EveryCompoundedCouponOfTheFixtureTelescopes) {
  const fixtures::CompareOis s = fixtures::make_compare_ois(full_options());
  int compounded = 0, telescoping = 0, days = 0;
  auto walk = [&](const instrument::Instrument& in) {
    for (const instrument::Leg& leg : in.legs) {
      for (const instrument::Coupon& c : leg.coupons) {
        if (c.kind != instrument::CouponKind::RfrCompounded) continue;
        ++compounded;
        days += c.obs_end - c.obs_begin;
        if (spike::telescopes(leg, c)) ++telescoping;
      }
    }
  };
  for (const instrument::CalibrationInstrument& ci : s.set.instruments) walk(ci.instrument);
  for (const instrument::Instrument& in : s.book) walk(in);
  std::cout << "[spike/telescoping] compare_ois at sixteen calibration instruments and sixteen book trades (D64): "
            << compounded << " compounded coupons carrying " << days << " projected observation days, of which "
            << telescoping << " coupons satisfy the telescoping precondition exactly\n";
  EXPECT_EQ(telescoping, compounded) << "a compounded coupon does not telescope; the spike would silently "
                                        "fall back to the naive form and measure nothing";
  EXPECT_GT(compounded, 0);
}

// ---------------------------------------------------------------------------------------------
// 2. The harness is neutral: the spike's naive column IS `record_compare_ois`.
// ---------------------------------------------------------------------------------------------
TEST(SpikeTelescoping, TheNaiveColumnReproducesTheEnginesOwnRecording) {
  const fixtures::CompareOis s = fixtures::make_compare_ois(small_options());
  const fixtures::CompareOisTape engine = fixtures::record_compare_ois(s);
  const fixtures::CompareOisTape mine = spike::record_compare_ois_spike(s, spike::Form::naive);
  // The counts are STRUCTURAL and must be exact (PRINCIPLES.md §4): they are what the spike
  // reports, and if the two harnesses recorded different graphs the telescoped column would be
  // measuring the harness. The VALUES are compared by tolerance and not bitwise, because this
  // test's TU is not pinned to `-ffp-contract=off` and `src/fixtures/compare_ois_e0.cpp` is.
  EXPECT_EQ(mine.nodes_raw, engine.nodes_raw);
  EXPECT_EQ(mine.nodes_after_passes, engine.nodes_after_passes);
  ASSERT_EQ(mine.record_pv.size(), engine.record_pv.size());
  double worst = 0.0;
  for (std::size_t i = 0; i < mine.record_pv.size(); ++i) {
    worst = std::max(worst, rel(mine.record_pv[i], engine.record_pv[i], std::fabs(engine.record_pv[i])));
  }
  worst = std::max(worst, rel(mine.record_book, engine.record_book, std::fabs(engine.record_book)));
  ASSERT_EQ(mine.record_knots.size(), engine.record_knots.size());
  for (std::size_t k = 0; k < mine.record_knots.size(); ++k) {
    worst = std::max(worst, rel(mine.record_knots[k], engine.record_knots[k], std::fabs(engine.record_knots[k])));
  }
  std::cout << "[spike/telescoping] the spike's naive column against record_compare_ois: " << mine.nodes_raw
            << " tape nodes recorded and " << mine.nodes_after_passes
            << " after the passes on both sides, worst relative value difference " << worst << "\n";
  EXPECT_LT(worst, 1e-12);
}

// ---------------------------------------------------------------------------------------------
// 3. The two forms agree on O1, O2 and O3 (PRINCIPLES.md §4: path-to-path, loose, not bitwise).
// ---------------------------------------------------------------------------------------------
TEST(SpikeTelescoping, TheTwoFormsAgreeOnTheOutputs) {
  const fixtures::CompareOis s = fixtures::make_compare_ois(small_options());
  fixtures::CompareOisTape na = spike::record_compare_ois_spike(s, spike::Form::naive);
  fixtures::CompareOisTape te = spike::record_compare_ois_spike(s, spike::Form::telescoped);
  ASSERT_TRUE(na.record_report.converged);
  ASSERT_TRUE(te.record_report.converged);

  // O1: the calibrated knots.
  ASSERT_EQ(na.record_knots.size(), te.record_knots.size());
  double worst_knot = 0.0;
  for (std::size_t k = 0; k < na.record_knots.size(); ++k) {
    worst_knot = std::max(worst_knot, rel(na.record_knots[k], te.record_knots[k], std::fabs(na.record_knots[k])));
  }
  // O2: the book.
  const double scale = std::fabs(na.record_book);
  const double worst_book = rel(na.record_book, te.record_book, scale);

  // O3: the ladder through the IFT.
  solver::ImplicitProgram pn(na.tape, na.registry, fixtures::compare_ois_program_options(s));
  solver::ImplicitProgram pt(te.tape, te.registry, fixtures::compare_ois_program_options(s));
  const std::vector<double> rn = fixtures::compare_ois_ladder(pn, s.quotes, {na.book_output}, nullptr);
  const std::vector<double> rt = fixtures::compare_ois_ladder(pt, s.quotes, {te.book_output}, nullptr);
  ASSERT_EQ(rn.size(), rt.size());
  double inf = 0.0;
  for (double v : rn) inf = std::max(inf, std::fabs(v));
  ASSERT_GT(inf, 0.0);
  double worst_ladder = 0.0;
  for (std::size_t k = 0; k < rn.size(); ++k) worst_ladder = std::max(worst_ladder, rel(rn[k], rt[k], inf));

  std::cout << "[spike/telescoping] naive against telescoped on six calibration instruments and eight book trades "
               "(D64): knots "
            << worst_knot << ", book PV " << worst_book << " relative to " << scale << ", O3 ladder " << worst_ladder
            << " relative to its infinity norm " << inf << "\n";
  EXPECT_LT(worst_knot, 1e-9);
  EXPECT_LT(worst_book, 1e-9);
  EXPECT_LT(worst_ladder, 1e-6);
}

// ---------------------------------------------------------------------------------------------
// 4. Accuracy against the D72 oracle. PRINCIPLES.md §4 demotes `Wide` to a diagnostic and keeps
//    it for exactly this: two algebraically equal forms, and the question of which is closer to
//    truth. Reported, with one one-sided gate (telescoped must not be WORSE).
// ---------------------------------------------------------------------------------------------
TEST(SpikeTelescoping, AccuracyOfBothFormsAgainstTheOracle) {
  const fixtures::CompareOis s = fixtures::make_compare_ois(full_options());
  const std::vector<double>& z = s.generating_z;   // a fixed curve: no solve, so only the coupon form differs

  const Priced<double> nd = price_at<double>(s, z, spike::Form::naive);
  const Priced<double> td = price_at<double>(s, z, spike::Form::telescoped);
  const Priced<Wide> nw = price_at<Wide>(s, z, spike::Form::naive);
  const Priced<Wide> tw = price_at<Wide>(s, z, spike::Form::telescoped);

  // First: the identity itself, at 106 bits. If the two Wide columns disagree by more than
  // rounding at that width, the hand-written telescoped form is simply wrong and nothing below
  // means anything.
  double worst_identity = 0.0;
  for (std::size_t i = 0; i < nw.par.size(); ++i) {
    const Wide d = nw.par[i] - tw.par[i];
    worst_identity = std::max(worst_identity, std::fabs(d.value()) / std::fabs(nw.par[i].value()));
  }
  std::cout << "[spike/telescoping] the identity at 106 significand bits: worst relative disagreement between the "
               "naive and telescoped par rates is "
            << worst_identity << " over " << nw.par.size() << " calibration instruments\n";
  EXPECT_LT(worst_identity, 1e-25) << "the hand-written telescoped form is not the same function";

  auto report = [](const char* what, const std::vector<double>& a, const std::vector<double>& b,
                   const std::vector<Wide>& truth) {
    double wa = 0.0, wb = 0.0;
    int a_closer = 0, b_closer = 0, tie = 0;
    for (std::size_t i = 0; i < truth.size(); ++i) {
      const double t = truth[i].value();
      const double sc = std::fabs(t);
      const double ea = std::fabs((Wide(a[i]) - truth[i]).value()) / sc;
      const double eb = std::fabs((Wide(b[i]) - truth[i]).value()) / sc;
      wa = std::max(wa, ea);
      wb = std::max(wb, eb);
      if (ea < eb) {
        ++a_closer;
      } else if (eb < ea) {
        ++b_closer;
      } else {
        ++tie;
      }
    }
    std::cout << "[spike/telescoping] " << what << " against truth: naive worst " << wa << ", telescoped worst " << wb
              << " (" << (wb == 0.0 ? 0.0 : wa / wb) << "x); closer to truth -- naive " << a_closer << ", telescoped "
              << b_closer << ", equal " << tie << " of " << truth.size() << "\n";
    return std::pair<double, double>(wa, wb);
  };

  std::vector<double> nd_par, td_par, nd_pv, td_pv;
  for (double v : nd.par) nd_par.push_back(v);
  for (double v : td.par) td_par.push_back(v);
  for (double v : nd.pv) nd_pv.push_back(v);
  for (double v : td.pv) td_pv.push_back(v);
  const auto par_err = report("calibration par rates", nd_par, td_par, nw.par);
  const auto pv_err = report("per-trade book PV", nd_pv, td_pv, nw.pv);

  const double tb = nw.book.value();
  const double eb_naive = std::fabs((Wide(nd.book) - nw.book).value()) / std::fabs(tb);
  const double eb_tel = std::fabs((Wide(td.book) - nw.book).value()) / std::fabs(tb);
  std::cout << "[spike/telescoping] book PV against truth: naive " << eb_naive << ", telescoped " << eb_tel << "\n";
  std::cout << "[spike/telescoping] truth book PV " << std::setprecision(17) << tb << ", naive double " << nd.book
            << ", telescoped double " << td.book << std::setprecision(6) << "\n";

  // One-sided, and loose: the telescoped form removes 249 roundings of 250 and is EXPECTED to be
  // the more accurate one, but a 3x allowance keeps this from being a flake on a fixture whose
  // errors are already at 1e-16.
  EXPECT_LE(par_err.second, par_err.first * 3.0 + 1e-18) << "the telescoped par rates are materially less accurate";
  EXPECT_LE(pv_err.second, pv_err.first * 3.0 + 1e-18) << "the telescoped trade PVs are materially less accurate";
}

// ---------------------------------------------------------------------------------------------
// 5. The prize, structurally: what survives of the recorded ratio after the tape passes, domain
//    inference and planning. Reported only -- this is the measurement the spike exists for.
// ---------------------------------------------------------------------------------------------
TEST(SpikeTelescoping, TheStructuralPrize) {
  const fixtures::CompareOis s = fixtures::make_compare_ois(full_options());
  const spike::FormCounts n = spike::count_compare_ois_spike(s, spike::Form::naive);
  const spike::FormCounts t = spike::count_compare_ois_spike(s, spike::Form::telescoped);
  std::cout << spike::compare_forms(n, t);
  // Both tapes' record-time calibrations converged and landed on the same book, at FULL size --
  // otherwise the telescoped column is a smaller program computing a different thing.
  EXPECT_TRUE(n.converged);
  EXPECT_TRUE(t.converged);
  EXPECT_LT(n.jtr_inf, 1e-12);
  EXPECT_LT(t.jtr_inf, 1e-12);
  const double book_rel = std::fabs(n.book_pv - t.book_pv) / std::fabs(n.book_pv);
  std::cout << "[spike/telescoping] at full size the two forms' record-point book PV agree to " << book_rel
            << " relative (naive " << std::setprecision(17) << n.book_pv << ", telescoped " << t.book_pv
            << std::setprecision(6) << "); optimality |J^T r|_inf " << n.jtr_inf << " and " << t.jtr_inf << "\n";
  EXPECT_LT(book_rel, 1e-9);
  EXPECT_LT(t.tape_nodes_after_passes, n.tape_nodes_after_passes)
      << "telescoping did not remove a single surviving tape node; the spike is not measuring what it thinks";
  EXPECT_GT(n.ir_scan_domains, 0u) << "the naive form should record the compounding as a scan (D41)";
  // The compounding scan is what telescoping removes, and its ROWS are the measure of it: the
  // telescoped form keeps a scan domain or two (the fixed leg's own folds are recurrences too)
  // but almost no rows in them. A tenfold drop is a floor, not the measurement -- the measurement
  // is the table printed above.
  EXPECT_LT(t.ir_scan_rows * 10u, n.ir_scan_rows) << "the compounding scan did not collapse";
}
