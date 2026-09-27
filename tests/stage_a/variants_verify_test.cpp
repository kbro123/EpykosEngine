// M3/G5: the scheme sweep — the Stage A problem recorded again with the SOFR curve on each
// variant of blueprints/curves/usd.json (log-DF, monotone cubic, the three-region composite),
// on a reduced book: every block converges, the IR round-trips, the sharing gate holds, the
// program at the record point agrees with the double maths at its solved knots to a measured
// tolerance, and the O6 select exports exist exactly where the scheme has a Select (the Hyman
// limiter of the monotone regions) with masks in {0, 1} and finite margins and arm gaps.
//
// The oracle comparison was bitwise until the algebra phase landed; PRINCIPLES.md §5.2a case 2
// re-anchors it as a measured tolerance. See the comment at the comparison.
//
// KNOWN FAILING on MonotoneCubic and Composite, and it is a PRINCIPLES.md §5.2a CASE 1 DEFECT in
// exec::Interpreter, not arithmetic and not a reason to loosen anything:
//
//   exec::Interpreter::run LEAVES SOME OUTPUTS UNWRITTEN. When a fused reduction domain has one
//   IR value carrying more than one output ordinal — pv(i) and pv_usd(i) share a node for a USD
//   trade — Interpreter::Impl::build_group (src/exec/interpreter.cpp, the `if (g.fused)` block)
//   stores a single ordinal per row in `g.emit_ordinal[v - g.value_base]`, so the second ordinal
//   found overwrites the first and only the LAST is emitted. Impl::decide_fusion then skips BOTH
//   ordinals of that value for the late copy, because it keys on `emitted[v]`, so the losing
//   ordinal is written nowhere and the caller reads back whatever its buffer held (zero, through
//   fixtures::run_lanes).
//
//   Measured on USD-SOFR-MONOTONE, 300 trades, release: 17 of 2,279 outputs never written, all
//   in one 13,660-row domain, 19 of 300 pv outputs read back as exactly 0 through ImplicitProgram
//   while the scalar replay of the SAME tape at the SAME state is correct and ir::evaluate agrees
//   with the replay. USD-SOFR-COMPOSITE: 88 of 300. It predates the algebra phase and is exposed
//   by it (with standard_passes() alone the same problem has 0 unwritten), it needs the book big
//   enough (0 at 60 trades, 9 at 150, 17 at 300) and it is batch-dependent (COMPOSITE: 0 unwritten
//   at max_batch 1, 87 at max_batch 8). The base Stage A problem is unaffected at 300 and at
//   2,000 trades and at max_batch 1, 8 and 64, which is why only this file shows it.
//
// The fix belongs in src/exec/interpreter.cpp and is not made here.
#include <gtest/gtest.h>

#include <algorithm>
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
#include "epykos/tape/replay.hpp"
#include "epykos/verify/differential.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace solver = epykos::solver;
using epykos::test::lane_knots;

namespace {

// The gate for the book against the templated `double` maths (PRINCIPLES.md §5.2a case 2),
// MEASURED before it was set: 1.12597e-13 on USD-SOFR-LOGDF, and 7.66075e-14 on the two
// Select-bearing variants over the outputs the interpreter actually writes (see the head
// comment: it does not write all of them). About an order of magnitude looser.
constexpr double kOracleRel = 1.0e-12;

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
  // The program at the quotes against the double maths at the solved knots; the exports.
  //
  // THIS WAS A BITWISE COMPARISON of every PV and the book total. PRINCIPLES.md §5.2a case 2: the
  // algebra phase (`epykos::compile`) collapses the recorded coupon, so `price_stage_a_at` on
  // double is a different rounding of the same real number. Measured and printed; a PV is a
  // difference of two legs and is measured against the leg scale (D26).
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 8));
  const std::vector<double> out = fixtures::run_lanes(prog, {s.quotes});
  for (int k = 0; k < prog.n_blocks(); ++k) EXPECT_TRUE(prog.report(k, 0).converged) << variant << " block " << k;
  const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, lane_knots(prog, t, 0));
  double worst = 0.0;
  for (int i = 0; i < s.n_trades(); ++i) {
    const std::size_t is = static_cast<std::size_t>(i);
    const double legs = std::max(std::fabs(book.leg0[is]), std::fabs(book.leg1[is]));
    worst = std::max(worst, epykos::verify::relative_error(out[static_cast<std::size_t>(t.layout.pv(i))], book.pv[is], legs));
  }
  worst = std::max(worst, epykos::verify::relative_error(out[static_cast<std::size_t>(t.layout.book)], book.book_total));
  std::cout << "[  oracle  ] " << variant << ": worst relative error vs the double maths " << worst << " (gate " << kOracleRel << ")\n";
  EXPECT_LT(worst, kOracleRel) << variant;
  // Case 1 (PRINCIPLES.md §5.2a): the scalar replay of the SAME pinned tape at the state the
  // program solved to. Both sides below the pin, so this is bitwise — and it is the gate that
  // names the defect in the head comment rather than letting it show up as a loose oracle error.
  std::vector<double> solved(prog.full_state(0), prog.full_state(0) + prog.n_inputs());
  const std::vector<double> replayed = epykos::replay(t.tape, solved);
  std::size_t vs_replay = 0;
  int first = -1;
  for (int o = 0; o < prog.n_outputs(); ++o) {
    if (epykos::test::bits(out[static_cast<std::size_t>(o)]) == epykos::test::bits(replayed[static_cast<std::size_t>(o)])) continue;
    if (first < 0) first = o;
    ++vs_replay;
  }
  EXPECT_EQ(vs_replay, 0u) << variant << ": the interpreter and the scalar replay of the same pinned tape disagree at the same state, first at output " << first
                           << " (interpreter " << (first < 0 ? 0.0 : out[static_cast<std::size_t>(first)]) << ", replay "
                           << (first < 0 ? 0.0 : replayed[static_cast<std::size_t>(first)]) << ")";
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
