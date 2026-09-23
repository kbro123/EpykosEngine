// M3/G5: the scheme sweep — the Stage A problem recorded again with the SOFR curve on each
// variant of blueprints/curves/usd.json (log-DF, monotone cubic, the three-region composite),
// on a reduced book: every block converges, the IR round-trips, the sharing gate holds, the
// program at the record point is bitwise the double maths at its solved knots, and the O6
// select exports exist exactly where the scheme has a Select (the Hyman limiter of the
// monotone regions) with masks in {0, 1} and finite margins and arm gaps.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/sharing.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace solver = epykos::solver;
using epykos::test::bits;
using epykos::test::lane_knots;

namespace {

// The O1 gate ‖Jᵀr‖∞ < 1e-12 is written for zero-rate unknowns. On log-DF unknowns the front
// instruments' Jacobian entries are 1/τ (the overnight deposit: 360), so at the residuals'
// rounding floor of ~5e-14 the diagnostic sits near 2e-11: the gate is scaled by that 1/τ for a
// variant whose front knots are log-DF (stated).
double jtr_gate(const std::string& variant) { return variant == "USD-SOFR-LOGDF" ? 360.0 * 1e-12 : 1e-12; }

void check_variant(const std::string& variant, bool expect_selects) {
  fixtures::StageAOptions o;
  o.usd_curve = variant;
  o.trades = 300;
  o.scenarios = 4;
  const fixtures::StageA s = fixtures::make_stage_a(o);
  const fixtures::StageATape t = fixtures::record_stage_a(s);
  std::cout << "[  " << variant << " ] " << t.stats.to_string() << '\n';
  for (std::size_t k = 0; k < t.record_reports.size(); ++k) {
    std::cout << "[  block   ] " << k << ": " << solver::to_string(t.record_reports[k]) << '\n';
    EXPECT_TRUE(t.record_reports[k].converged) << variant << " block " << k;
    EXPECT_LT(t.record_reports[k].jtr_inf, jtr_gate(variant)) << variant << " block " << k;
  }
  EXPECT_EQ(t.layout.selects.n() > 0, expect_selects) << variant << ": " << t.layout.selects.n() << " selects";
  // Round trip.
  ir::InferStats stats;
  const ir::Program p = ir::infer(t.tape, &stats);
  EXPECT_NO_THROW(ir::validate(p));
  const epykos::Tape expanded = ir::expand(p);
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(t.tape, expanded, &diff)) << variant << ":\n" << diff;
  const fixtures::StageAStructure st = fixtures::stage_a_structure(p);
  std::cout << "[  struct  ] " << st.to_string() << (stats.scan_retries.empty() ? "" : "; retries:" + stats.scan_retries) << '\n';
  EXPECT_GE(st.scan_domains, 1u);
  EXPECT_EQ(st.select_domains > 0, expect_selects);
  // Sharing.
  const ir::SharingReport rep = ir::sharing(p, t.sharing_groups(), epykos::Op::Exp);
  std::string why;
  EXPECT_TRUE(ir::assert_all_shared(rep, &why)) << variant << ": " << why;
  EXPECT_EQ(ir::duplicates(p, epykos::Op::Exp), 0u) << variant;
  std::cout << "[  sharing ] " << rep.matching.size() << " DF domains, all shared: " << ir::assert_all_shared(rep) << '\n';
  // The program at the quotes: bitwise the double maths at the solved knots; the exports.
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 8));
  const std::vector<double> out = fixtures::run_lanes(prog, {s.quotes});
  for (int k = 0; k < prog.n_blocks(); ++k) EXPECT_TRUE(prog.report(k, 0).converged) << variant << " block " << k;
  const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, lane_knots(prog, t, 0));
  std::size_t mismatches = 0;
  for (int i = 0; i < s.n_trades(); ++i) mismatches += bits(out[static_cast<std::size_t>(t.layout.pv(i))]) != bits(book.pv[static_cast<std::size_t>(i)]);
  mismatches += bits(out[static_cast<std::size_t>(t.layout.book)]) != bits(book.book_total);
  EXPECT_EQ(mismatches, 0u) << variant;
  int flips_possible = 0;
  for (int x = 0; x < t.layout.selects.n(); ++x) {
    const double mask = t.layout.selects.mask(x, out.data(), 1, 0), margin = t.layout.selects.margin(x, out.data(), 1, 0), gap = t.layout.selects.gap(x, out.data(), 1, 0);
    EXPECT_TRUE(mask == 0.0 || mask == 1.0) << variant << " select " << x;
    EXPECT_TRUE(std::isfinite(margin) && std::isfinite(gap)) << variant << " select " << x;
    flips_possible += std::fabs(margin) < 1e-6;
  }
  std::cout << "[  o6      ] " << t.layout.selects.n() << " selects exported; " << flips_possible << " within 1e-6 of a flip at the record point; book "
            << out[static_cast<std::size_t>(t.layout.book)] << " vs the base problem's " << variant << '\n';
}

}  // namespace

TEST(StageAVariants, LogDf) { check_variant("USD-SOFR-LOGDF", false); }
TEST(StageAVariants, MonotoneCubic) { check_variant("USD-SOFR-MONOTONE", true); }
TEST(StageAVariants, Composite) { check_variant("USD-SOFR-COMPOSITE", true); }
