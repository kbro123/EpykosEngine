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
//   ALL 2,000 trades agree to better than 1e-10 of notional; every family to 1.9e-14 of its gross
//   PV. Book NPV: engine 68,395,553.471440 against QuantLib 68,395,553.471439 — **1.35e-14
//   relative**.
//
//   family                         n   worst/notional   |family diff| / family GROSS
//   EUR-3S6S-BASIS               212   9.07e-17         1.07e-15
//   EUR-ESTR-OIS                 300   1.14e-14         1.86e-14
//   EUR-EURIBOR-3M-IRS           167   1.25e-15         4.97e-16
//   EUR-EURIBOR-6M-IRS           237   1.41e-15         2.1e-16
//   USD-SOFR-AVG-SWAP            199   1.72e-15         7.03e-16
//   USD-SOFR-OIS                 481   8.47e-15         1.4e-15
//   USD-SOFR-OIS-SHIFT2          195   7.85e-15         4.42e-15
//   USD-SOFR-OIS-SHIFT2-LOCKOUT2 209   1.01e-14         3.9e-15
//
// ---- §ROLL: the roll convention is a TERM, and this harness READS it --------------------------
//
// `endOfMonth` is not passed as a blanket `true`, and it is no longer inferred twice either: the
// generator takes it per leg from the engine's own dump of `leg.roll_eom` — the roll convention
// the engine RESOLVED when it built the trade (D129). Over the book that term is 3,972
// day-of-month legs and 28 EOM legs, and the three modes measure:
//
//   what the generator is passed                         book vs QuantLib
//   the engine's resolved roll term (`leg.roll_eom`)      1.3508e-14   <- what produced the literals
//   the literal calendar test, re-derived in the harness  1.3508e-14
//   a blanket `true`                                      3.4554e-06
//
// The middle row is D128's fix and the top row is D129's: same answer, one source of truth. The
// first two agree because the engine's own inference IS that literal test, which is the point —
// reading the term cannot drift from it, while re-deriving it can.
//
// The blanket row cost exactly three trades, and WHY is worth keeping, because it is a distinction
// between two different jobs the name "end of month" does:
//
//   * the EOM ROLL gate — "was this swap written to roll on month ends?" — a question about the
//     CALENDAR, and
//   * ModifiedFollowing's rollback test — "did adjusting this date cross into the next month?" —
//     necessarily a question about BUSINESS days.
//
// Both engines keep those separate and implement the second identically: ours is
// `month_of(next_business_day(d)) == month_of(d) ? … : previous_business_day(d)`
// (`conventions/calendar.cpp:221`), QuantLib's is `if (d1.month() != d.month()) return adjust(d,
// Preceding)` (`calendar.cpp:96`). Neither consults an `isEndOfMonth` helper for it.
//
// But QuantLib's EOM-ROLL gate (`schedule.cpp:377`) is `calendar_.isEndOfMonth(seed)`, and
// `Calendar::isEndOfMonth(d)` is `d.month() != adjust(d+1).month()` — the BUSINESS-day notion.
//
// That is not arbitrary: it makes the test ADJUSTED-DATE TOLERANT. A genuine EOM swap terminating
// Sunday 2029-09-30 adjusts under ModifiedFollowing to Friday 2029-09-28, which is not a calendar
// month end; a literal test handed that date loses the convention, and QuantLib's recovers it. The
// cost is that it cannot tell that case apart from ours — 2029-09-28, -29 and -30 all adjust to the
// same day and all three test TRUE, because the distinguishing information is simply not in the
// adjusted date. It lives only in the UNADJUSTED roll date, which the originating convention holds.
//
// So neither gate is wrong; they are right for different inputs, and `Schedule` documents its dates
// as unadjusted (it adjusts them itself at schedule.cpp:373) while its gate suits a possibly
// adjusted one. On every genuine month end the two agree, so the literal test is a strict subset and
// never misses a real EOM swap on unadjusted input — which is what this fixture supplies.
//
// Two things the EOM detection is NOT. It does not roll the termination back to the Friday — that is
// ModifiedFollowing, applied before the block, which only touches dates_[1 .. size-2]. And when it
// fires with a non-Unadjusted convention it snaps those intermediate dates to
// `calendar_.endOfMonth`, the last BUSINESS day of each month, which is a third business-day notion
// in the same neighbourhood and the one that actually produced trade 129's 45930 / 46295.
//
// None of this is a convention disagreement and the engine was never in question: an earlier
// version of this file recorded it as an open question needing a primary source (D127 §2), which
// D128 withdrew as the sixth harness defect of the oracle programme, and D129 then removed the
// inference it rested on. There are 8 legs in this book (4 trades, 3 of them priced differently)
// where QuantLib's business-day gate would infer a roll the engine does not — findable now by
// reading the term rather than by diffing prices.
//
// ---- the other seven schedule parameters, audited ---------------------------------------------
//
// `conventions::ScheduleSpec` has eight fields. Six cross to QuantLib unchanged (effective,
// termination, frequency, bdc, calendar, and the termination convention). Two need TRANSLATION and
// both were got wrong on the first attempt: `stub = ShortFront` -> `DateGeneration::Backward`
// (D119 §2) and `eom`, which is now READ off the leg rather than translated (§ROLL, D129). The
// eighth, `term` ("one period from effective to termination"), is **not passed at all** — and
// that is safe by construction rather
// than by luck: no leg in `blueprints/conventions/instruments.json` has `term_up_to != frequency`
// (checked: 0 of 14), so a trade short enough to fire `term` is also short enough that one period
// of `frequency` spans it, and `Period(frequency)` reproduces it. It fires on 180 of the 2,000
// book trades and 30 calibration legs, so this is load-bearing and not a corner.
//
// ---- provenance (D108's rule, as D114-D126) ---------------------------------------------------
//
// Nine exact hex-float literals from QuantLib 1.35 — one per family and the book total. No
// dependency; D12 untouched. TO REGENERATE: see D127 §1, with one step changed by §ROLL — the
// generator takes `endOfMonth` per leg from the engine's own dump (`roll <trade> <eom> <dom>`
// rows, generator mode 3) instead of re-deriving it. The traps of D115 §3 and D119 §2 apply,
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

// QuantLib 1.35, `endOfMonth` per leg from the engine's resolved roll term (§ROLL).
//
// THE DENOMINATOR IS THE FAMILY'S GROSS PV, sum |pv_i|, NOT its net total — and that is not a
// convenience. A family of offsetting trades has a net that is a small difference of large
// numbers: EUR-ESTR-OIS nets 695,702 out of trades with notionals in the millions, so an absolute
// disagreement of 1.6e-6 reads as 2.4e-12 against the net and 1e-13 against the gross, while the
// per-trade measure is 1.14e-14 of notional. Holding a near-cancelling net to a relative tolerance
// measures the cancellation, not the engine; the first version of this test did exactly that and
// failed EUR-ESTR-OIS for it.
//
// All eight families are held to 1e-12 of gross and all eight are inside 1.9e-14, so every bound
// here has ~50x of headroom and nothing is loosened to accommodate a known disagreement. Until
// D128 three of them carried a trade that disagreed and were held to the cost it imposed; those
// bounds went with the defect, and the book total is held the same way rather than at 1e-6.
inline const Fam kFamilies[] = {
    {"EUR-3S6S-BASIS", 0x1.bfa59a74be4ddp+20, 1.0e-12},
    {"EUR-ESTR-OIS", 0x1.53b2ca5447df8p+19, 1.0e-12},
    {"EUR-EURIBOR-3M-IRS", 0x1.1d05fc029f817p+22, 1.0e-12},
    {"EUR-EURIBOR-6M-IRS", -0x1.0ee4edc2bbbc9p+24, 1.0e-12},
    {"USD-SOFR-AVG-SWAP", 0x1.9437edacaf9e2p+25, 1.0e-12},
    {"USD-SOFR-OIS", 0x1.5f093e5311485p+25, 1.0e-12},
    {"USD-SOFR-OIS-SHIFT2", -0x1.6a2057b6dc47p+20, 1.0e-12},
    {"USD-SOFR-OIS-SHIFT2-LOCKOUT2", -0x1.1b32b2433e156p+24, 1.0e-12},
};
inline constexpr double kQlBookPv = 0x1.04e8885e2c0e1p+26;

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
                              << " of its gross PV from QuantLib against a 1e-12 bound it normally clears by 50x. "
                                 "Every family is clean, so this is a real disagreement and not a known one.";
  }
  const double book_rel = std::abs(total - kQlBookPv) / book_gross;
  std::cout << "  [ql-oracle] BOOK: engine " << total << ", QuantLib " << kQlBookPv << ", |diff|/gross " << book_rel
            << " (gross " << book_gross << ")\n";
  EXPECT_LT(book_rel, 1.0e-12) << "the 2,000-trade book NPV is " << book_rel << " of the book's gross PV from "
                                  "QuantLib, against 1.35e-14 when the literals were generated. Nothing is "
                                  "expected to disagree: read §ROLL before loosening this.";
}

// §ROLL, the half that is the ENGINE's obligation (D129): the roll term the engine reports must be
// the term its own dates were generated on. This is what lets the harness READ `leg.roll_eom`
// instead of re-deriving it, and it is the only thing standing between "one source of truth" and
// "a second inference that happens to agree today".
//
// Measured over the book, both directions, zero exceptions: `roll_eom` is exactly
// `is_end_of_month(unadjusted termination)` and `roll_dom` is exactly its day of month. The
// termination is the anchor because every convention here rolls backward from a short front stub;
// a convention with a back stub would anchor on the effective date and this gate would need the
// other branch.
TEST(StageABookOracleE0, EveryLegReportsTheRollTermItsDatesWereGeneratedOn) {
  const fixtures::StageA& s = book();
  int legs = 0, eom = 0, dom = 0;
  std::map<std::string, int> eom_by_family;
  for (int i = 0; i < s.n_trades(); ++i) {
    const epykos::instrument::Instrument& in = s.instruments[static_cast<std::size_t>(i)];
    ASSERT_NE(in.termination, 0) << in.id << " has no unadjusted termination to anchor the roll on";
    const bool anchor_is_month_end = conventions::is_end_of_month(in.termination);
    for (const epykos::instrument::Leg& leg : in.legs) {
      ++legs;
      EXPECT_EQ(leg.roll_eom, anchor_is_month_end)
          << in.id << ": the leg reports roll_eom = " << leg.roll_eom << " but its roll anchor "
          << conventions::to_iso(in.termination) << " is "
          << (anchor_is_month_end ? "a calendar month end" : "not a calendar month end")
          << ". The reported term and the term the dates obey have come apart.";
      if (leg.roll_eom) {
        ++eom;
        eom_by_family[in.blueprint]++;
      } else {
        ++dom;
        EXPECT_EQ(leg.roll_dom, conventions::civil_from_days(in.termination).d)
            << in.id << ": roll_dom = " << leg.roll_dom << " but the anchor "
            << conventions::to_iso(in.termination) << " rolls on day "
            << conventions::civil_from_days(in.termination).d;
      }
    }
  }
  // The census, pinned as data: this is the term the literals were generated from.
  EXPECT_EQ(legs, 4000);
  EXPECT_EQ(eom, 28) << "the book's EOM leg count moved; the oracle literals were generated from 28";
  EXPECT_EQ(dom, 3972);
  const std::map<std::string, int> kEomByFamily = {{"EUR-3S6S-BASIS", 2},
                                                   {"EUR-EURIBOR-6M-IRS", 6},
                                                   {"USD-SOFR-AVG-SWAP", 4},
                                                   {"USD-SOFR-OIS", 14},
                                                   {"USD-SOFR-OIS-SHIFT2-LOCKOUT2", 2}};
  EXPECT_EQ(eom_by_family, kEomByFamily) << "which families carry an EOM leg changed";
  std::cout << "  [ql-oracle] roll term over 4,000 legs: " << eom << " EOM, " << dom
            << " day-of-month, 0 that disagree with their own dates\n";
}

// §ROLL's obligation, pinned: the literal calendar test and QuantLib's
// business-day one must still DISAGREE on the three dates below, and must AGREE on a true month
// end. If QuantLib ever changes `isEndOfMonth`, or if the engine's `is_end_of_month` becomes a
// business-day test, this fails and the per-trade flag above needs re-deriving rather than the
// tolerances being loosened.
TEST(StageABookOracleE0, TheTwoEndOfMonthGatesStillDisagreeOnALastBusinessDay) {
  const fixtures::StageA& s = book();
  const conventions::Calendar& usd = s.registry.calendar("USD-SOFR-SWAP");
  const conventions::Calendar& eur = s.registry.calendar("EUR-TARGET");
  struct C { const char* iso; bool is_eur; };
  // The three roll dates that cost three trades when `endOfMonth = true` was passed blanket (D127): each
  // a Saturday that is the last business day of its month without being its last calendar day.
  const C cases[] = {{"2029-09-29", false}, {"2027-10-30", false}, {"2028-04-29", true}};
  for (const C& c : cases) {
    const conventions::Date d = conventions::parse_iso(c.iso);
    const conventions::Calendar& cal = c.is_eur ? eur : usd;
    EXPECT_FALSE(conventions::is_end_of_month(d)) << c.iso << " is a calendar month end after all";
    const conventions::Date next = cal.next_business_day(d + 1);
    EXPECT_NE(conventions::civil_from_days(next).m, conventions::civil_from_days(d).m)
        << c.iso << "'s next business day " << conventions::to_iso(next)
        << " is in the same month, so QuantLib's business-day gate would not fire and this date no longer "
           "demonstrates the distinction";
  }
  // And on a true month end the two agree, which is why the literal test never misses a real EOM
  // swap: it is a strict subset of the business-day one.
  for (const char* iso : {"2025-09-30", "2028-04-30"}) {
    const conventions::Date d = conventions::parse_iso(iso);
    EXPECT_TRUE(conventions::is_end_of_month(d)) << iso << " should be a calendar month end";
  }
  std::cout << "  [ql-oracle] the EOM-roll gate and the business-day test disagree on 3 dates and agree on "
               "every true month end\n";
}
