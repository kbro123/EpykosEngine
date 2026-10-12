// The EUR-EURIBOR IRS fixed-leg day-count DISAGREEMENT, priced and bounded.
//
// `blueprints/conventions/instruments.json` records, against both `EUR-EURIBOR-3M-IRS` and
// `EUR-EURIBOR-6M-IRS`:
//
//     "cross_check": "disagree"
//     "notes": ["fixed-leg day count: SwapEngine 30E/360 vs 30/360 (CFTC MAT, Bloomberg SEF,
//               Strata); registry 30/360"]
//
// D117 §1 found this is one of three convention questions the import left open, and that **every**
// convention, calendar and index in the registry came from the other engine — so the head-to-head
// cannot adjudicate any of them. D68's method says measure the consequence before fitting a
// solution, and that is what this file does. The answer turns out to be sharp.
//
// MEASURED (D118 §1): Stage A's 54 par-rate quotes were computed twice, the second time against a
// blueprints tree with both fixed legs flipped to 30E/360 (via `$EPYKOS_BLUEPRINTS`, so no code
// changed, and the override was verified to reach the loaded registry). **Not one quote moved by
// more than 1e-9 bp.** The disagreement is worth exactly nothing on the problem this engine
// benchmarks.
//
// WHY, structurally — this is the part worth keeping, because it says where the question COULD
// matter. The two bases differ in one place only: 30/360 (US, Bond Basis) maps a day-31 end to 30
// **only when the start is the 30th or 31st**, while 30E/360 maps every day-31 end to 30
// unconditionally. So they disagree exactly on a period whose END is the 31st and whose START is
// not the 30th or 31st. Two consequences:
//
//   * A swap rolling on the 31st every period is UNAFFECTED, because both bases map the 31st
//     start to the 30th and both give exactly 1.0 per annual period. The `eom: true` on these
//     conventions therefore does not expose the question.
//   * Only a STUB can expose it — a front or back period that ends on a 31st having begun
//     mid-month. Stage A's EURIBOR swaps roll annually from spot and are stub-free, which is why
//     the measurement is a clean zero rather than a small number.
//
// So this test pins both halves: the two bases really are distinct in this engine (otherwise the
// zero above would be measuring nothing), and Stage A's fixed-leg accruals never end on a 31st
// (which is why the zero holds). A future fixture that adds a stub will fail the second half, and
// that is the point: it is the signal that the question has become live and needs a primary source.
#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/conventions/date.hpp"
#include "epykos/conventions/daycount.hpp"
#include "epykos/conventions/registry.hpp"
#include "epykos/fixtures/stage_a.hpp"

namespace conventions = epykos::conventions;
namespace fixtures = epykos::fixtures;

// Half one: the two bases are genuinely different in this engine, and differ exactly where the
// definitions say. Without this, the measured zero could be two names for one function.
TEST(IrsDayCountDisagreement, TheTwoBasesDifferExactlyOnADayThirtyOneEndFromAMidMonthStart) {
  struct Case {
    const char* from;
    const char* to;
    bool differs;
    const char* why;
  };
  const Case cases[] = {
      {"2026-05-15", "2027-05-31", true, "end is the 31st, start is mid-month: the one disagreeing shape"},
      {"2026-08-31", "2027-08-31", false, "both bases map a 31st START to the 30th"},
      {"2026-01-30", "2027-01-31", false, "start is the 30th, so 30/360 US also maps the end to 30"},
      {"2026-09-25", "2027-09-27", false, "neither endpoint is a 31st -- Stage A's shape"},
      {"2026-02-28", "2027-02-28", false, "February is not special to either basis"},
  };
  for (const Case& c : cases) {
    const conventions::Date d1 = conventions::parse_iso(c.from), d2 = conventions::parse_iso(c.to);
    const double us = conventions::year_fraction(conventions::DayCount::Thirty360US, d1, d2);
    const double e30 = conventions::year_fraction(conventions::DayCount::ThirtyE360, d1, d2);
    EXPECT_EQ(us != e30, c.differs) << c.from << " -> " << c.to << ": " << c.why << " (30/360 " << us
                                    << ", 30E/360 " << e30 << ')';
  }
}

// The registry still holds the reading this repository chose, with the disagreement recorded. If
// someone resolves the question, this fails and asks for the decision entry.
TEST(IrsDayCountDisagreement, TheRegistryStillHoldsThirtyThreeSixtyOnBothEuriborIrs) {
  const conventions::Registry r = conventions::Registry::load();
  for (const char* name : {"EUR-EURIBOR-3M-IRS", "EUR-EURIBOR-6M-IRS"}) {
    const conventions::InstrumentConvention& c = r.instrument(name);
    bool saw_fixed = false;
    for (const conventions::LegConvention& leg : c.legs) {
      if (!leg.index.empty()) continue;
      saw_fixed = true;
      EXPECT_EQ(leg.day_count, conventions::DayCount::Thirty360US)
          << name << "'s fixed leg is no longer 30/360. SwapEngine reads 30E/360 and the registry records the "
             "disagreement as unresolved; if it has been resolved, say so in a decision entry and update this "
             "test with the primary source.";
    }
    EXPECT_TRUE(saw_fixed) << name << " has no fixed leg";
  }
}

// Half two: and Stage A never produces the shape that would expose it. This is why the measured
// difference over all 54 quotes is exactly zero, and it is the half that will fail first if a
// future fixture adds a stub.
TEST(IrsDayCountDisagreement, NoStageAFixedAccrualEndsOnTheThirtyFirst) {
  const fixtures::StageA s = fixtures::make_stage_a();
  std::size_t fixed_coupons = 0, exposed = 0, reported = 0;
  for (int c = 0; c < s.n_curves(); ++c) {
    for (const epykos::instrument::CalibrationInstrument& ci : s.sets[static_cast<std::size_t>(c)].instruments) {
      for (const epykos::instrument::Leg& leg : ci.instrument.legs) {
        if (!leg.index.empty()) continue;  // the fixed leg is the one whose basis is disputed
        if (leg.day_count != conventions::DayCount::Thirty360US) continue;
        for (const epykos::instrument::Coupon& cp : leg.coupons) {
          ++fixed_coupons;
          const int end_dom = conventions::day_of_month(cp.end);
          const int start_dom = conventions::day_of_month(cp.start);
          if (end_dom != 31 || start_dom >= 30) continue;
          if (reported < 8) {
            ADD_FAILURE() << ci.key << ": a 30/360 fixed accrual runs " << conventions::to_iso(cp.start) << " -> "
                          << conventions::to_iso(cp.end)
                          << ", which 30/360 and 30E/360 value DIFFERENTLY. The day-count disagreement recorded "
                             "against the EURIBOR IRS conventions is now LIVE on this fixture and its cost must be "
                             "measured and the convention resolved against a primary source.";
            ++reported;
          }
          ++exposed;
        }
      }
    }
  }
  EXPECT_EQ(exposed, 0u);
  // And the scan saw something, so a refactor that stops finding 30/360 fixed legs fails here
  // rather than passing vacuously.
  EXPECT_GT(fixed_coupons, 0u) << "no 30/360 fixed coupons found in Stage A's calibration sets at all, so this test "
                                  "is now vacuous -- the EURIBOR IRS rows have moved or changed basis";
  std::cout << "  [daycount] " << fixed_coupons << " 30/360 fixed accruals in Stage A's calibration sets, " << exposed
            << " of a shape where 30/360 and 30E/360 differ\n";
}
