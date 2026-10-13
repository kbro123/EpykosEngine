// A SEASONED trade against QuantLib: the realised prefix of a straddling coupon, which until now
// no oracle anywhere reached.
//
// D124 §4 named this the largest uncovered area, and D123 is the reason it mattered. The convention
// error D123 found lived in the back half of the compounded-SOFR formula — which day count the
// annualised rate is applied over. The realised prefix is the FRONT half of the same formula: a
// period straddling the valuation date compounds published fixings up to today and forwards after
// it, and `realised_observations` / `coupon_rate`'s `realised_factor` seam is where the two meet.
// `compare_ois` cannot reach it: it is unseasoned by construction, which is one of its four
// deliberate agreement controls (compare_ois.hpp reason 4). Stage A's book is 20% seasoned.
//
// The trade: Stage A's trade 39, a 10-year USD SOFR OIS seasoned by 52 days, paying fixed on a
// notional of 99,563,730. Its current coupon runs 2026-08-03 → 2027-08-02 and carries **51 realised
// calendar days** (a `realised_factor` of 1.005437477536187 over ~36 published business days) ahead
// of 212 projected observations — QuantLib counts 248 business days in the period, which is the
// same decomposition read the other way.
//
//   current coupon rate   engine vs QuantLib   1.31e-11 bp
//   trade PV              engine vs QuantLib   5.01e-14 relative
//
// ---- what makes this an independent check rather than a shared one ---------------------------
//
// The FIXINGS are the engine's own synthetic series (D16: deterministic from the seed, labelled
// synthetic, not market data), and they are handed to QuantLib's index verbatim via `addFixings`.
// That is the right direction of travel: the published history is an INPUT to both engines, like
// the curve and the trade, so sharing it is sharing a fixture and not sharing an implementation.
// What is compared is what each engine does WITH it — how many of those fixings fall in the period,
// which calendar days each carries, how they compound, and where the realised prefix hands over to
// the projection.
//
// Only fixings strictly before the evaluation date are loaded, which is the same rule
// `realised_observations` applies; feeding QuantLib a fixing dated today would make it use a
// published rate where the engine projects one, and the disagreement would be the harness's.
//
// ---- provenance (D108's rule, as D114-D124) ---------------------------------------------------
//
// Two exact hex-float literals from QuantLib 1.35. No dependency; D12 untouched.
// TO REGENERATE: dump Stage A's USD-SOFR fixing series and slot 0's generating curve daily, build
// the trade as `OvernightIndexedSwap(side, notional, Schedule(eff, UNADJUSTED term, Annual,
// join(SIFMA, FED), ModifiedFollowing, ModifiedFollowing, DateGeneration::Backward, eom), rate,
// Actual360(), Sofr(h), 0.0, 2, ModifiedFollowing, cal, false, Compound)`, load the pre-valuation
// fixings with `addFixings`, and price with `DiscountingSwapEngine`. The traps of D115 §3 and
// D119 §2 apply unchanged.
//
// An E0 TU (-ffp-contract=off in every preset), and in the mutation gate set by name.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"
#include "epykos/maths/instrument/coupon.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fixtures = epykos::fixtures;

namespace {

constexpr int kTrade = 39;
inline constexpr double kQlSeasonedPv = 0x1.2b9b868338dfp+21;
inline constexpr double kQlSeasonedCurrentCouponRate = 0x1.3e9157ee27693p-5;

// Three orders above the measured 5.01e-14 / 1.31e-11 bp.
constexpr double kPvRelTol = 1.0e-10;
constexpr double kRateTolBp = 1.0e-8;

const fixtures::StageA& exact_par_stage_a() {
  static const fixtures::StageA s = [] {
    fixtures::StageAOptions o;
    o.quote_noise_bp = 0.0;
    return fixtures::make_stage_a(o);
  }();
  return s;
}

}  // namespace

// The trade the literals belong to: seasoned, with a straddling first coupon that really does carry
// realised days. Asserted first, because every number below is meaningless if the trade stopped
// being seasoned — and the fixture draws its book from a seeded stream, so trade 39's identity is a
// fact to check rather than assume.
TEST(SeasonedOracleE0, TradeThirtyNineIsStillASeasonedOisWithARealisedPrefix) {
  const fixtures::StageA& s = exact_par_stage_a();
  ASSERT_GT(s.n_trades(), kTrade);
  const fixtures::StageATrade& t = s.trades[static_cast<std::size_t>(kTrade)];
  EXPECT_TRUE(t.seasoned) << "trade " << kTrade << " is no longer seasoned";
  EXPECT_EQ(t.blueprint, "USD-SOFR-OIS");
  EXPECT_EQ(t.age_days, 52);

  const epykos::instrument::Instrument& in = s.instruments[static_cast<std::size_t>(kTrade)];
  std::size_t realised_legs = 0;
  for (const epykos::instrument::Leg& leg : in.legs) {
    if (leg.index.empty()) continue;
    ASSERT_FALSE(leg.coupons.empty());
    const epykos::instrument::Coupon& cp = leg.coupons.front();
    EXPECT_TRUE(cp.current) << "the first coupon no longer straddles the valuation date";
    EXPECT_GT(cp.fixed_days, 0) << "the straddling coupon has no realised days, so the realised "
                                   "prefix this test exists to check is empty";
    EXPECT_GT(cp.realised_factor, 1.0) << "a realised factor of " << cp.realised_factor
                                       << " means no published fixing compounded into this coupon";
    EXPECT_GT(cp.obs_end - cp.obs_begin, 0) << "the coupon has no PROJECTED days either, so it is "
                                               "fully fixed and tests the handover of nothing";
    ++realised_legs;
    std::cout << "  [ql-oracle] trade " << kTrade << " current coupon: " << cp.fixed_days
              << " realised calendar days, realised_factor " << cp.realised_factor << ", "
              << (cp.obs_end - cp.obs_begin) << " projected observations\n";
  }
  EXPECT_EQ(realised_legs, 1u);
  // The history it compounds from must actually be there.
  EXPECT_GT(s.fixings.size("USD-SOFR"), 100u) << "the fixings history is too short to season anything";
}

// The oracle. Both numbers come from QuantLib pricing the same trade on the same curve with the
// same published fixings; what differs is each engine's handling of the realised prefix.
TEST(SeasonedOracleE0, TheSeasonedTradeAndItsStraddlingCouponMatchQuantLib) {
  const fixtures::StageA& s = exact_par_stage_a();
  const epykos::instrument::Instrument& in = s.instruments[static_cast<std::size_t>(kTrade)];
  const auto df = [&s](int slot, double t) { return s.generating_df(slot, t); };

  double pv = 0.0;
  for (std::size_t L = 0; L < in.legs.size(); ++L) {
    const double sign = (L == 0 ? 1.0 : -1.0) * static_cast<double>(in.side);
    pv += sign * epykos::instrument::leg_pv<double>(in.legs[L], df);
  }
  const double pv_rel = std::abs(pv - kQlSeasonedPv) / std::abs(kQlSeasonedPv);
  EXPECT_LT(pv_rel, kPvRelTol) << "engine PV " << pv << ", QuantLib " << kQlSeasonedPv << ", relative " << pv_rel;

  double rate = 0.0;
  for (const epykos::instrument::Leg& leg : in.legs) {
    if (leg.index.empty()) continue;
    rate = epykos::instrument::coupon_rate<double>(leg, leg.coupons.front(), df);
    break;
  }
  const double rate_bp = (rate - kQlSeasonedCurrentCouponRate) * 1.0e4;
  EXPECT_LT(std::abs(rate_bp), kRateTolBp)
      << "the straddling coupon's rate is " << rate_bp
      << " bp from QuantLib. The realised prefix and the projection meet in coupon_rate's "
         "realised_factor seam; read this file's header on which fixings are loaded before "
         "concluding the engine is wrong.";
  std::cout << "  [ql-oracle] seasoned trade PV " << pv_rel << " relative, straddling coupon rate " << rate_bp
            << " bp vs QuantLib\n";
}

// The comparison is sensitive to the realised prefix, which is the whole point: drop the published
// history and the coupon's rate must move by far more than the tolerance. Without this, a test that
// passed because BOTH engines ignored the fixings would look identical to one that passed because
// both used them.
TEST(SeasonedOracleE0, TheComparisonIsSensitiveToTheRealisedFixings) {
  const fixtures::StageA& s = exact_par_stage_a();
  const epykos::instrument::Instrument& in = s.instruments[static_cast<std::size_t>(kTrade)];
  const auto df = [&s](int slot, double t) { return s.generating_df(slot, t); };

  for (const epykos::instrument::Leg& leg : in.legs) {
    if (leg.index.empty()) continue;
    const epykos::instrument::Coupon& real = leg.coupons.front();
    const double with = epykos::instrument::coupon_rate<double>(leg, real, df);
    // The same coupon with its realised prefix neutralised: a factor of 1 and no fixed days. The
    // projected observations are untouched, so the difference is exactly the realised contribution.
    epykos::instrument::Coupon without = real;
    without.realised_factor = 1.0;
    without.realised_sum = 0.0;
    without.fixed_days = 0;
    const double bare = epykos::instrument::coupon_rate<double>(leg, without, df);
    const double moved_bp = std::abs(with - bare) * 1.0e4;
    std::cout << "  [ql-oracle] neutralising the realised prefix moves the coupon rate by " << moved_bp << " bp\n";
    EXPECT_GT(moved_bp, 1.0e3 * kRateTolBp)
        << "the realised fixings contribute almost nothing to this coupon, so the oracle above is "
           "not actually constraining them";
    break;
  }
}
