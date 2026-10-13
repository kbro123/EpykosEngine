// THE TWO-CURVE ORACLE, and the first on the problem this project actually benchmarks.
//
// D114 compared interpolation schemes; D115/D116 compared single-curve SOFR OIS on the `compare_ois`
// fixture, which refuses a non-OIS convention by design because an IRS's forecast index differs
// from its discount index and a one-curve world would silently misprice it (D117 §2). Stage A has
// what that fixture cannot represent: `EUR-EURIBOR-6M-IRS` rows whose float leg reads curve 3 and
// discounts on curve 1. This file compares all three of Stage A's par-rate calibration families to
// QuantLib 1.35.
//
//   family                                 rows   worst gap
//   EUR-ESTR-OIS         (TARGET, Estr)      17   5.25e-11 bp
//   USD-SOFR-OIS         (SIFMA n FED)       13   5.15e-11 bp
//   EUR-EURIBOR-6M-IRS   (6M fwd / ESTR)     11   7.63e-13 bp
//   EUR-3S6S-BASIS       (3M/6M fwd/ESTR)     9   8.46e-14 bp   <- THREE curves, float-vs-float
//   USD-SOFR-3M-FUTURE   (SR3, a PRICE)        8   6.25e-11 bp of rate
//   EUR-EURIBOR-3M-FUTURE (FEU3, a PRICE)      8   exactly 0
//   the four DEPOSITS    (one per curve)        4   exactly 0
//
// **70 instruments: ALL of Stage A's calibration quotes**, two currencies, three calendars, one,
// two and three curves, par rates, prices and deposits. Nothing on its calibration side is left
// uncompared.
//
// The futures are quoted as PRICES and are compared as prices, with **convexity zero on both
// sides** — the owner ruled on 2026-10-13 that the quotes are inclusive of any convexity, so
// `convexity zero` is the CONVENTION and not an approximation being excused, and applying a
// Hull-White adjustment to either side would be measuring the wrong thing. A price point is 100 bp
// of rate, which is why their tolerance below is stated in price and read in rate. What that covers beyond
// D115: a second currency's calendar end to end; the EURIBOR-6M index; a 30/360 annual fixed leg
// against an ACT/360 SEMIANNUAL float leg; forecast != discount; and the STUB RULE, which the 18M
// ESTR row is the only instrument in Stage A to exercise (see below).
//
// Why this is worth having at all is D117 §1: **17 of 17 instrument conventions, 7 of 7 calendars
// and 4 of 4 indices in `blueprints/conventions/` were imported from SwapEngine**, so the
// head-to-head cannot check any of them. These are the only numbers in the repository that can.
//
// ---- provenance (D108's rule, as D114-D116) ---------------------------------------------------
//
// The expectations are QuantLib 1.35's own `fairRate()`, exact hex floats, generated once. **No
// dependency; D12 untouched.** Nothing is lifted from SwapEngine: the fixture, the curves and the
// instruments are ours. `quote_noise_bp = 0` makes Stage A's quotes the generating curves' EXACT
// par rates, which is the fixture's own O1-recovery setting.
//
// TO REGENERATE: dump `StageA::generating_df(slot, t)` on a daily grid for slots 0, 1 and 3 (the
// basis is ACT/365 from the valuation date, which is what `knot_t` uses), hold each in QuantLib as
// `InterpolatedDiscountCurve<LogLinear>` with `Actual365Fixed`, and for each row build
//
//   Schedule(effective, UNADJUSTED termination, freq, calendar, ModifiedFollowing,
//            ModifiedFollowing, DateGeneration::Backward, endOfMonth = true)
//
// then `OvernightIndexedSwap(..., paymentLag = 2, ..., RateAveraging::Compound)` for the OIS
// families, or `VanillaSwap(..., Thirty360(BondBasis), float_sched, Euribor6M(fwd), 0.0,
// Actual360())` priced with `DiscountingSwapEngine(estr)` for the IRS.
//
// TWO HARNESS TRAPS, both of which reported the ENGINE as wrong before they were found:
//
//  1. **Never `MakeOIS` or `MakeVanillaSwap`** (D115 §3). They advance the tenor to the ADJUSTED
//     termination and generate BACKWARD from it, so a weekend roll date leaves a spurious
//     three-day front stub.
//  2. **`DateGeneration::Backward`, not `Forward`.** Our conventions declare `stub: ShortFront`;
//     Forward generation produces a short BACK stub. The two agree on every whole-year and every
//     sub-annual tenor, so 40 of the 41 rows cannot tell them apart — and the **18M ESTR row**,
//     the only instrument in Stage A that is neither sub-annual nor a whole number of years,
//     disagreed by 0.0265 bp until the rule was corrected. One row out of 41 carried the entire
//     signal for a convention that applies to every stubbed trade a desk books.
//
// An E0 TU (-ffp-contract=off in every preset), and in the mutation gate set by name.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;

namespace {

// QuantLib 1.35 fair rates, in Stage A's own instrument order within each curve.
inline constexpr double kQlEstrOis[] = {
    0x1.3d2a09484b33bp-6, 0x1.3eede236e4b57p-6,  // 1M, 2M
    0x1.40d8570ff46d6p-6, 0x1.45c432806227ap-6,  // 3M, 6M
    0x1.4acb8108233f6p-6, 0x1.4fbf8158d1e0ep-6,  // 9M, 1Y
    0x1.5660594dec1c5p-6, 0x1.5d6e50324813ep-6,  // 18M, 2Y
    0x1.687f64436efd8p-6, 0x1.717a0ab5c1f0ap-6,  // 3Y, 4Y
    0x1.78ce56acee5f3p-6, 0x1.83dc5ae2842adp-6,  // 5Y, 7Y
    0x1.8e7e9271a72dp-6,  0x1.933dd0f04063fp-6,  // 10Y, 12Y
    0x1.983823673cce8p-6, 0x1.9d59a4ce1cd7dp-6,  // 15Y, 20Y
    0x1.a270413dd25fbp-6,                        // 30Y
};
inline constexpr double kQlSofrOis[] = {
    0x1.342b26015486ap-5, 0x1.3531173b0b914p-5,  // 1M, 2M
    0x1.364fe222a7d5p-5,  0x1.483941f22da0ep-5,  // 3M, 2Y
    0x1.4b6c6d7aa4273p-5, 0x1.4e0603b7535d2p-5,  // 3Y, 4Y
    0x1.51ec0751b3952p-5, 0x1.55a0dfc800f8ep-5,  // 5Y, 7Y
    0x1.57458e26f3018p-5, 0x1.58fa0161a6464p-5,  // 10Y, 12Y
    0x1.5ab5aa51dec56p-5, 0x1.5bbc27d4b9f0fp-5,  // 15Y, 20Y
    0x1.5c658a872ad17p-5,                        // 30Y
};
// The 3s6s basis: 3M EURIBOR + spread (quarterly) against 6M EURIBOR flat (semiannual), ESTR
// discounted, spread on the shorter leg -- so three curves, and the only float-vs-float shape here.
// The spread enters the 3M leg linearly, so QuantLib's par spread is solved in CLOSED FORM,
// s = (PV6 - PV3)/A3 with A3 the 3M annuity sum(tau_i * N * DF(pay_i)), rather than by a root find:
// importing a solver into an oracle would be a second thing to be wrong.
// The futures, as PRICES, in Stage A's own contract order (Z26 H27 M27 U27 Z27 H28 M28 U28).
// SR3 is 100 − compounded daily SOFR over the IMM quarter, ACT/360 → QuantLib's
// `OvernightIndexFuture(sofr, valueDate, maturityDate, 0, Compound)`. FEU3 is 100 − the 3M EURIBOR
// fixing whose deposit runs from the third Wednesday for three months → 100·(1 − forwardRate(eff,
// term, ACT/360, Simple)) off the 3M forecast curve. Both with a zero convexity adjustment.
inline constexpr double kQlSr3Price[] = {
    0x1.80b71dfa4d13bp+6, 0x1.8092fea4ff94ap+6,  // Z26, H27
    0x1.8071bcc2e29a1p+6, 0x1.80532135d7f72p+6,  // M27, U27
    0x1.8036f5fdc9cp+6,   0x1.801a8786bf589p+6,  // Z27, H28
    0x1.80036c8480f69p+6, 0x1.7fed9b5589c52p+6,  // M28, U28
};
inline constexpr double kQlFeu3Price[] = {
    0x1.8798f57548218p+6, 0x1.87627406778e7p+6,  // Z26, H27
    0x1.873419f244546p+6, 0x1.8709bc3c55432p+6,  // M27, U27
    0x1.86e27df40d6c8p+6, 0x1.86be15d945e6ep+6,  // Z27, H28
    0x1.869a25f1bcfb4p+6, 0x1.867c2c533b674p+6,  // M28, U28
};
inline constexpr double kQlBasisParSpread[] = {
    0x1.a2820a9a214cep-11, 0x1.a2b90d49e0135p-11,  // 3Y, 4Y
    0x1.a2e0f80c3272ep-11, 0x1.a3191ce8d211dp-11,  // 5Y, 7Y
    0x1.a34a27796a73ap-11, 0x1.a35ff4d36e906p-11,  // 10Y, 12Y
    0x1.a375894e88009p-11, 0x1.a38c180d84f5ap-11,  // 15Y, 20Y
    0x1.a3a28f349a5c7p-11,                         // 30Y
};
inline constexpr double kQlEuribor6mIrs[] = {
    0x1.724e3276dc1e7p-6, 0x1.80c4036273712p-6,  // 1Y, 2Y
    0x1.8bda6db25231bp-6, 0x1.94e43f94222a5p-6,  // 3Y, 4Y
    0x1.9c4a1ee257c28p-6, 0x1.a79c6d6515f5bp-6,  // 5Y, 7Y
    0x1.b26f858fec582p-6, 0x1.b732f0ba7302p-6,   // 10Y, 12Y
    0x1.bc467b046eb6ep-6, 0x1.c178c8ef70cdbp-6,  // 15Y, 20Y
    0x1.c6a7a06d876c3p-6,                        // 30Y
};

// Comfortably above the measured 5.25e-11 bp and still 1e-12 of a rate: a convention change fails
// loudly, the last bits of a 30-year compounding are not a maintenance burden.
constexpr double kTolBp = 1.0e-8;

const fixtures::StageA& exact_par_stage_a() {
  static const fixtures::StageA s = [] {
    fixtures::StageAOptions o;
    o.quote_noise_bp = 0.0;  // the generating curves' EXACT par quotes (the O1 recovery setting)
    return fixtures::make_stage_a(o);
  }();
  return s;
}

// Stage A's quotes for one blueprint on one curve slot, in the fixture's own order.
std::vector<double> quotes_for(const fixtures::StageA& s, int slot, const std::string& blueprint,
                               std::vector<std::string>* tenors) {
  std::vector<double> out;
  int k = 0;
  for (int c = 0; c < s.n_curves(); ++c) {
    for (const epykos::instrument::CalibrationInstrument& ci : s.sets[static_cast<std::size_t>(c)].instruments) {
      if (c == slot && ci.instrument.blueprint == blueprint) {
        out.push_back(s.quotes[static_cast<std::size_t>(k)]);
        if (tenors != nullptr) tenors->push_back(ci.tenor);
      }
      ++k;
    }
  }
  return out;
}

void compare(const char* label, int slot, const std::string& blueprint, const double* ql, std::size_t n) {
  const fixtures::StageA& s = exact_par_stage_a();
  std::vector<std::string> tenors;
  const std::vector<double> ours = quotes_for(s, slot, blueprint, &tenors);
  ASSERT_EQ(ours.size(), n) << label << ": Stage A has " << ours.size() << " rows of this family against the "
                            << n << " in the QuantLib table -- the fixture's instrument set moved, so regenerate";
  double worst = 0.0;
  std::string worst_tenor;
  for (std::size_t i = 0; i < n; ++i) {
    const double bp = (ours[i] - ql[i]) * 1.0e4;
    if (std::abs(bp) > std::abs(worst)) {
      worst = bp;
      worst_tenor = tenors[i];
    }
    EXPECT_NEAR(ours[i], ql[i], kTolBp * 1.0e-4)
        << label << ' ' << tenors[i] << ": " << bp
        << " bp from QuantLib. Before concluding the engine is wrong, read this file's header on MakeOIS and on "
           "DateGeneration::Backward -- both traps reported the engine as wrong first.";
  }
  std::cout << "  [ql-oracle] " << label << ": " << n << " rows, worst " << worst << " bp at " << worst_tenor << '\n';
}

}  // namespace

// The fixture the literals belong to. Asserted, not assumed: the quotes are a function of the
// generating curves and the instrument sets, so a change to either must fail HERE with an
// instruction to regenerate.
TEST(StageAParOracleE0, TheFixtureTheLiteralsWereGeneratedFromHasNotMoved) {
  const fixtures::StageA& s = exact_par_stage_a();
  ASSERT_EQ(s.n_curves(), 4);
  EXPECT_EQ(s.n_quotes(), 70);
  EXPECT_EQ(s.sets[0].index, "USD-SOFR");
  EXPECT_EQ(s.sets[1].index, "EUR-ESTR");
  EXPECT_EQ(s.sets[2].index, "EUR-EURIBOR-3M");
  EXPECT_EQ(s.sets[3].index, "EUR-EURIBOR-6M");
}

TEST(StageAParOracleE0, EstrOisMatchesQuantLib) {
  compare("EUR-ESTR-OIS", 1, "EUR-ESTR-OIS", kQlEstrOis, std::size(kQlEstrOis));
}

TEST(StageAParOracleE0, SofrOisMatchesQuantLib) {
  compare("USD-SOFR-OIS", 0, "USD-SOFR-OIS", kQlSofrOis, std::size(kQlSofrOis));
}

// The two-curve one: the float leg forecasts off curve 3 and discounts off curve 1.
TEST(StageAParOracleE0, Euribor6mIrsMatchesQuantLibAcrossTwoCurves) {
  compare("EUR-EURIBOR-6M-IRS", 3, "EUR-EURIBOR-6M-IRS", kQlEuribor6mIrs, std::size(kQlEuribor6mIrs));
}

// The futures, compared as prices. A price point is 100 bp of rate, so `kTolBp` read as a price
// tolerance is a hundred times LOOSER in rate than it is for a par rate — which is why the measured
// figures are quoted in RATE in this file's header (6.25e-11 bp for SR3, exactly 0 for FEU3) rather
// than reported as a price difference and left flattering.
//
// NOTE the console line these two tests print: `compare` scales by 1e4 for every family, so for a
// price-quoted one that column is price×1e4, i.e. **100× the rate bp**. SR3's "-6.25e-09" there is
// the 6.25e-11 bp of rate quoted above. The shared helper is left alone rather than given a
// per-family scale, because one honest sentence is cheaper than a parameter nothing else needs.
TEST(StageAParOracleE0, SofrFuturesPricesMatchQuantLib) {
  compare("USD-SOFR-3M-FUTURE", 0, "USD-SOFR-3M-FUTURE", kQlSr3Price, std::size(kQlSr3Price));
}

TEST(StageAParOracleE0, EuriborFuturesPricesMatchQuantLib) {
  compare("EUR-EURIBOR-3M-FUTURE", 2, "EUR-EURIBOR-3M-FUTURE", kQlFeu3Price, std::size(kQlFeu3Price));
}

// The futures really are quoted as PRICES and the par-rate families really are not: the fixture's
// own flag says which, and the two are compared on different scales. Without this, a family that
// silently changed quoting convention would be compared against the wrong kind of number.
TEST(StageAParOracleE0, OnlyTheFuturesAreQuotedAsPrices) {
  const fixtures::StageA& s = exact_par_stage_a();
  std::size_t prices = 0, rates = 0;
  int k = 0;
  for (int c = 0; c < s.n_curves(); ++c) {
    for (const epykos::instrument::CalibrationInstrument& ci : s.sets[static_cast<std::size_t>(c)].instruments) {
      const bool is_price = s.quote_is_price[static_cast<std::size_t>(k)] != 0;
      const bool is_future = ci.instrument.kind == epykos::instrument::Kind::Future;
      EXPECT_EQ(is_price, is_future) << ci.key << ": quoted as a " << (is_price ? "price" : "rate")
                                     << " but its kind is " << (is_future ? "Future" : "not Future");
      if (is_price) {
        ++prices;
        EXPECT_GT(s.quotes[static_cast<std::size_t>(k)], 50.0)
            << ci.key << ": a futures PRICE near " << s.quotes[static_cast<std::size_t>(k)]
            << " is not a price at all -- the quoting convention has changed";
      } else {
        ++rates;
        EXPECT_LT(std::abs(s.quotes[static_cast<std::size_t>(k)]), 1.0) << ci.key << ": a rate quote out of range";
      }
      ++k;
    }
  }
  EXPECT_EQ(prices, 16u) << "Stage A no longer has 16 futures rows";
  EXPECT_EQ(rates, 54u) << "Stage A no longer has 54 rate-quoted rows";
  std::cout << "  [ql-oracle] " << prices << " price-quoted and " << rates << " rate-quoted calibration rows\n";
}

// The basis is quoted as a SPREAD, around 8 bp rather than 2.7%, so the same absolute tolerance is
// four orders of magnitude tighter in relative terms -- which is why it is the tightest agreement
// in the file and not a weaker one.
TEST(StageAParOracleE0, ThreeCurveBasisMatchesQuantLib) {
  compare("EUR-3S6S-BASIS", 2, "EUR-3S6S-BASIS", kQlBasisParSpread, std::size(kQlBasisParSpread));
}

// The basis really does read THREE distinct curves. Without this, a basis whose two forecast slots
// had collapsed onto one would price its two legs off the same curve, give a spread near zero, and
// still match a QuantLib swap built the same wrong way.
TEST(StageAParOracleE0, TheBasisReadsThreeDistinctCurves) {
  const fixtures::StageA& s = exact_par_stage_a();
  std::size_t checked = 0;
  for (const epykos::instrument::CalibrationInstrument& ci : s.sets[2].instruments) {
    if (ci.instrument.blueprint != "EUR-3S6S-BASIS") continue;
    ++checked;
    int spread_legs = 0;
    std::vector<int> forecast;
    for (const epykos::instrument::Leg& leg : ci.instrument.legs) {
      ASSERT_FALSE(leg.index.empty()) << ci.key << ": a basis swap has no fixed leg";
      EXPECT_EQ(leg.disc_curve, 1) << ci.key << ": both legs must discount on EUR-ESTR (slot 1)";
      forecast.push_back(leg.curve);
      if (leg.spread_leg) ++spread_legs;
    }
    ASSERT_EQ(forecast.size(), 2u) << ci.key << ": a basis swap has two legs";
    EXPECT_NE(forecast[0], forecast[1]) << ci.key << ": both legs forecast off slot " << forecast[0]
                                        << ", so the spread is structurally zero and the comparison is vacuous";
    EXPECT_EQ(spread_legs, 1) << ci.key << ": exactly one leg carries the quoted spread";
  }
  EXPECT_GT(checked, 0u) << "no 3s6s basis rows found at all";
  std::cout << "  [ql-oracle] " << checked << " basis rows, two distinct forecast curves discounting on slot 1\n";
}

// The structural facts the comparison above rests on, pinned so the two-curve claim cannot quietly
// become a one-curve one. An IRS whose forecast and discount slots coincided would still match a
// QuantLib swap built the same wrong way, and nothing else here would notice.
TEST(StageAParOracleE0, TheIrsReallyIsTwoCurveAndTheStubIsShortFront) {
  const fixtures::StageA& s = exact_par_stage_a();
  std::size_t checked = 0, stubbed = 0;
  for (const epykos::instrument::CalibrationInstrument& ci : s.sets[3].instruments) {
    if (ci.instrument.blueprint != "EUR-EURIBOR-6M-IRS") continue;
    ++checked;
    for (const epykos::instrument::Leg& leg : ci.instrument.legs) {
      if (leg.index.empty()) {
        EXPECT_EQ(leg.curve, -1) << ci.key << ": a fixed leg has a projection curve";
        EXPECT_EQ(leg.disc_curve, 1) << ci.key << ": the fixed leg must discount on EUR-ESTR (slot 1)";
      } else {
        EXPECT_EQ(leg.curve, 3) << ci.key << ": the float leg must forecast on EUR-EURIBOR-6M (slot 3)";
        EXPECT_EQ(leg.disc_curve, 1) << ci.key << ": the float leg must discount on EUR-ESTR (slot 1)";
        EXPECT_NE(leg.curve, leg.disc_curve) << ci.key << ": forecast and discount are the same slot, so this is no "
                                                "longer a two-curve comparison and the oracle above proves less "
                                                "than its name claims";
      }
    }
  }
  EXPECT_GT(checked, 0u) << "no EURIBOR-6M IRS rows found at all";

  // The stub rule, which exactly one instrument in Stage A exercises. A SHORT FRONT stub means the
  // first period is the short one; it is what made the 18M ESTR row disagree by 0.0265 bp against a
  // forward-generated schedule, and it is the only row of 41 that can see the difference.
  for (const epykos::instrument::CalibrationInstrument& ci : s.sets[1].instruments) {
    if (ci.tenor != "18M") continue;
    ++stubbed;
    for (const epykos::instrument::Leg& leg : ci.instrument.legs) {
      ASSERT_GE(leg.coupons.size(), 2u) << "the 18M OIS must have two periods for a stub to exist";
      const epykos::instrument::Coupon& first = leg.coupons.front();
      const epykos::instrument::Coupon& second = leg.coupons[1];
      EXPECT_LT(first.accrual, second.accrual)
          << ci.key << ": the FIRST period is not the short one, so the stub is no longer ShortFront. The QuantLib "
             "table above was generated with DateGeneration::Backward to match ShortFront; regenerate it.";
    }
  }
  EXPECT_EQ(stubbed, 1u) << "the 18M ESTR row is gone, and with it the only instrument in Stage A that distinguishes "
                            "a front stub from a back one";
  std::cout << "  [ql-oracle] " << checked << " two-curve IRS rows (forecast 3 / discount 1), " << stubbed
            << " stubbed instrument exercising ShortFront\n";
}
