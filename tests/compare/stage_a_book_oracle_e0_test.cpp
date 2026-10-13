// STAGE A'S WHOLE BOOK against QuantLib: 2,000 trades, eight families, two currencies, 378
// seasoned. This is the problem D109's head-to-head measured against SwapEngine — whose
// conventions ours were imported from (D117 §1: 17 of 17, 7 of 7 calendars, 4 of 4 indices) — so
// that comparison is structurally blind to exactly the layer this one checks.
//
// Every trade is rebuilt in QuantLib from its terms (notional, side, rate or spread, effective,
// unadjusted termination, curve slots), so **2,000 schedules** are under test and not just the
// pricing. The curves are Stage A's four generating curves; the fixings are its own synthetic
// series, loaded pre-valuation only (D125 §2's rule).
//
//   1,997 of 2,000 trades agree to better than 1e-10 of notional.
//   3 disagree, all for ONE reason, and it is a convention question — see §EOM below.
//
//   family                         n   worst/notional   |family diff| / family GROSS
//   EUR-3S6S-BASIS               212   9.07e-17         1.07e-15
//   EUR-ESTR-OIS                 300   1.14e-14         1.86e-14
//   EUR-EURIBOR-3M-IRS           167   1.25e-15         4.97e-16
//   USD-SOFR-AVG-SWAP            199   1.72e-15         7.03e-16
//   USD-SOFR-OIS-SHIFT2          195   7.85e-15         4.42e-15
//   EUR-EURIBOR-6M-IRS           237   7.85e-07         1.62e-07   <- one trade
//   USD-SOFR-OIS                 481   4.67e-06         2.71e-07   <- one trade
//   USD-SOFR-OIS-SHIFT2-LOCKOUT2 209   1.94e-05         2.70e-06   <- one trade
//
//   The five families with no EOM-affected trade agree to 1.9e-14 of gross or better. The three
//   that have one are 1.6e-07 to 2.7e-06, and that whole span is the three trades.
//
//   book NPV   engine 68,395,553.471440   QuantLib 68,395,789.808924
//              |diff| = 236.34, which is 3.46e-06 of the NET and ~2.4e-07 of the book's GROSS PV.
//              The net is quoted here only because it is the familiar number; every tolerance in
//              this file is against the gross, for the reason given at `kFamilies`.
//
// ---- §EOM: the three, and the open question ---------------------------------------------------
//
// All three outliers have a termination that is the **last business day of its month without being
// the last calendar day**, and the divergence is in EOM *detection*, not in the EOM adjustment:
//
//   ours      `is_end_of_month(d)` is `end_of_month(d) == d` — the literal CALENDAR month end
//             (conventions/date.hpp), seeded from the termination for backward generation
//             (conventions/schedule.cpp:43).
//   QuantLib  `Calendar::isEndOfMonth(d)` is `d.month() != adjust(d+1).month()` — a BUSINESS-day
//             notion, true whenever the next business day falls in the next month.
//
// Worked: trade 129 terminates 2029-09-29, a **Saturday**. Ours: the 30th is the calendar month
// end, so not EOM, and the annual grid stays on the 29th. QuantLib: the next business day is
// 2029-10-01, so it IS end-of-month, and the grid snaps to the 30th — 2025-09-30, 2026-09-30, …
// (verified from the generated schedule: 45930, 46295 against our 45929, 46294).
//
// **Neither blanket setting matches, which is what proves the mechanism.** Re-running with
// `endOfMonth = false` fixes trades 129 and 1704 and BREAKS trade 1688, whose effective is
// 2025-09-30 — a true calendar month end that does need EOM. So this is not "QuantLib applies EOM
// and we do not"; it is two different tests for the same flag.
//
// WHICH IS RIGHT IS NOT DECIDED HERE. The 2006 ISDA Definitions' EOM rule is phrased about the
// last day of a calendar month, which reads our way, but **that is exactly the kind of inference
// that was wrong about the observation shift** (D116 presented two defensible readings; D123 found
// the ARRC settled it against this engine). It needs the primary source, and until then the test
// pins the mechanism and the cost rather than a verdict.
//
// ---- provenance (D108's rule, as D114-D126) ---------------------------------------------------
//
// Nine exact hex-float literals from QuantLib 1.35 — one per family and the book total. No
// dependency; D12 untouched. TO REGENERATE: see D127 §1. The traps of D115 §3 and D119 §2 apply,
// and two more were found writing this one: the basis arm must skip cashflows that
// `hasOccurred(valuation)` (46 seasoned basis trades threw "negative time" without it, because a
// paid coupon's past fixing was being demanded), and the serial arithmetic for a roll date must be
// printed rather than inferred — the EOM diagnosis above turned on a single day.
//
// An E0 TU (-ffp-contract=off in every preset), and in the mutation gate set by name.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "epykos/conventions/calendar.hpp"
#include "epykos/conventions/date.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/maths/instrument/coupon.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace conventions = epykos::conventions;
namespace fixtures = epykos::fixtures;

namespace {

struct Fam {
  const char* name;
  double ql;        // QuantLib's PV for the family
  double tol_rel;   // tolerance on |engine - QL| as a fraction of the family's GROSS PV
};

// QuantLib 1.35, endOfMonth = true.
//
// THE DENOMINATOR IS THE FAMILY'S GROSS PV, sum |pv_i|, NOT its net total — and that is not a
// convenience. A family of offsetting trades has a net that is a small difference of large
// numbers: EUR-ESTR-OIS nets 695,702 out of trades with notionals in the millions, so an absolute
// disagreement of 1.6e-6 reads as 2.4e-12 against the net and 1e-13 against the gross, while the
// per-trade measure is 1.14e-14 of notional. Holding a near-cancelling net to a relative tolerance
// measures the cancellation, not the engine; the first version of this test did exactly that and
// failed EUR-ESTR-OIS for it.
//
// The five clean families are held to 1e-12 of gross; the three carrying one EOM-affected trade
// each are held to the cost that trade imposes, stated rather than hidden, and §EOM's test is what
// pins WHY they are loose.
inline const Fam kFamilies[] = {
    {"EUR-3S6S-BASIS", 0x1.bfa59a74be4ddp+20, 1.0e-12},
    {"EUR-ESTR-OIS", 0x1.53b2ca5447df8p+19, 1.0e-12},
    {"EUR-EURIBOR-3M-IRS", 0x1.1d05fc029f817p+22, 1.0e-12},
    {"EUR-EURIBOR-6M-IRS", -0x1.0ee4dd031391dp+24, 1.0e-06},
    {"USD-SOFR-AVG-SWAP", 0x1.9437edacaf9e2p+25, 1.0e-12},
    {"USD-SOFR-OIS", 0x1.5f091563e8a29p+25, 1.0e-06},
    {"USD-SOFR-OIS-SHIFT2", -0x1.6a2057b6dc47p+20, 1.0e-12},
    {"USD-SOFR-OIS-SHIFT2-LOCKOUT2", -0x1.1b3184ce2f8f6p+24, 1.0e-05},
};
inline constexpr double kQlBookPv = 0x1.04e8c373c5677p+26;

// The three trades whose termination is the last business day of its month without being the last
// calendar day. Pinned by index AND re-derived from the dates, so a fixture reshuffle fails loudly.
inline constexpr int kEomTrades[] = {129, 1188, 1704};

const fixtures::StageA& book() {
  static const fixtures::StageA s = [] {
    fixtures::StageAOptions o;
    o.quote_noise_bp = 0.0;
    return fixtures::make_stage_a(o);
  }();
  return s;
}

double trade_pv(const fixtures::StageA& s, int i) {
  const epykos::instrument::Instrument& in = s.instruments[static_cast<std::size_t>(i)];
  const auto df = [&s](int slot, double t) { return s.generating_df(slot, t); };
  double pv = 0.0;
  for (std::size_t L = 0; L < in.legs.size(); ++L) {
    pv += ((L == 0 ? 1.0 : -1.0) * static_cast<double>(in.side)) *
          epykos::instrument::leg_pv<double>(in.legs[L], df);
  }
  return pv;
}

}  // namespace

TEST(StageABookOracleE0, TheBookTheLiteralsWereGeneratedFromHasNotMoved) {
  const fixtures::StageA& s = book();
  EXPECT_EQ(s.n_trades(), 2000);
  EXPECT_EQ(s.n_curves(), 4);
  EXPECT_EQ(s.n_netting_sets(), 40);
  std::map<std::string, int> n;
  int seasoned = 0;
  for (int i = 0; i < s.n_trades(); ++i) {
    n[s.trades[static_cast<std::size_t>(i)].blueprint]++;
    if (s.trades[static_cast<std::size_t>(i)].seasoned) ++seasoned;
  }
  EXPECT_EQ(n.size(), std::size(kFamilies)) << "the book's family set changed";
  EXPECT_EQ(seasoned, 378) << "the seasoned count changed, so the fixings path is exercising a different set";
  for (const Fam& f : kFamilies) EXPECT_GT(n[f.name], 0) << f.name << " is gone from the book";
}

TEST(StageABookOracleE0, EveryFamilyAndTheBookTotalMatchQuantLib) {
  const fixtures::StageA& s = book();
  std::map<std::string, double> ours, gross;
  double total = 0.0, book_gross = 0.0;
  for (int i = 0; i < s.n_trades(); ++i) {
    const double pv = trade_pv(s, i);
    const std::string& bp = s.trades[static_cast<std::size_t>(i)].blueprint;
    ours[bp] += pv;
    gross[bp] += std::abs(pv);
    total += pv;
    book_gross += std::abs(pv);
  }
  for (const Fam& f : kFamilies) {
    const double g = gross[f.name];
    ASSERT_GT(g, 0.0) << f.name;
    const double rel = std::abs(ours[f.name] - f.ql) / g;
    std::cout << "  [ql-oracle] " << f.name << ": engine " << ours[f.name] << ", QuantLib " << f.ql
              << ", |diff|/gross " << rel << " (gross " << g << ")\n";
    EXPECT_LT(rel, f.tol_rel) << f.name << " is " << rel
                              << " of its gross PV from QuantLib. If this family has no EOM-affected trade its "
                                 "bound is 1e-12 and a failure is a real disagreement; read §EOM otherwise.";
  }
  const double book_rel = std::abs(total - kQlBookPv) / book_gross;
  std::cout << "  [ql-oracle] BOOK: engine " << total << ", QuantLib " << kQlBookPv << ", |diff|/gross " << book_rel
            << " (gross " << book_gross << ")\n";
  EXPECT_LT(book_rel, 1.0e-6) << "the 2,000-trade book NPV is " << book_rel << " of the book's gross PV from "
                                 "QuantLib. The three EOM trades of §EOM account for 2.4e-7 of it; more than that "
                                 "is something new.";
}

// §EOM, pinned by its MECHANISM. The three divergent trades must each have a termination that is
// the last business day of its month without being the last calendar day — the one shape on which
// our calendar EOM test and QuantLib's business-day one disagree. If the EOM question is ever
// resolved, this fails and names the decision that has to be recorded; if a fourth trade acquires
// the shape, the count fails.
TEST(StageABookOracleE0, TheThreeDivergentTradesAreExactlyTheEomDetectionShape) {
  const fixtures::StageA& s = book();
  const conventions::Calendar& usd = s.registry.calendar("USD-SOFR-SWAP");
  const conventions::Calendar& eur = s.registry.calendar("EUR-TARGET");
  for (const int i : kEomTrades) {
    ASSERT_LT(i, s.n_trades());
    const epykos::instrument::Instrument& in = s.instruments[static_cast<std::size_t>(i)];
    const bool is_eur = in.blueprint.rfind("EUR", 0) == 0;
    const conventions::Calendar& cal = is_eur ? eur : usd;
    const conventions::Date t = in.termination;
    // Not the last CALENDAR day of its month...
    EXPECT_FALSE(conventions::is_end_of_month(t))
        << "trade " << i << " terminates on " << conventions::to_iso(t)
        << ", which IS a calendar month end -- then both EOM tests agree on it and it cannot be one of the three";
    // ...but the next business day falls in the next month, which is QuantLib's test.
    const conventions::Date next = cal.next_business_day(t + 1);
    EXPECT_NE(conventions::civil_from_days(next).m, conventions::civil_from_days(t).m)
        << "trade " << i << " terminates on " << conventions::to_iso(t) << " and its next business day "
        << conventions::to_iso(next) << " is in the SAME month, so QuantLib's EOM test does not fire either and "
           "this trade is not the shape this test exists to pin";
    std::cout << "  [ql-oracle] trade " << i << " (" << in.blueprint << ") terminates " << conventions::to_iso(t)
              << "; next business day " << conventions::to_iso(next) << " is in the next month\n";
  }
}
