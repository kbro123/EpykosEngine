// EpykosEngine — the symbolic-algebraic-reduction DESIGN headers compile, and their enumerations
// are complete (P1, D73; docs/TERM_REWRITING.md).
//
// This package ships an INTERFACE, not an implementation, so there is no behaviour to test. What
// there IS to gate:
//
//   1. Every new header compiles on its own, under every preset and both compilers. D46's lesson
//      applies to headers nobody has instantiated yet as much as to any other: a header that has
//      never been through GCC 13 is a header that does not compile.
//   2. Every `Axiom` and `RecurrenceClass` has a distinct name. The identity table in §3 and the
//      enum are two statements of the same list, and the cheapest guard against them drifting is
//      that adding an enumerator without a name fails here.
//   3. The classification predicates partition, and no axiom sits in two buckets.
//   4. Nothing in the interface lets a rewrite be admitted BECAUSE it preserves bits.
//      PRINCIPLES.md §4 retired that on 2026-09-25; these tests stop it creeping back as a default.
//   5. The bounds that must not be relaxed silently are off by default.
//
// Nothing here constructs an `EGraph` or calls any declared-but-undefined function: they are
// undefined on purpose until the interface has been reviewed.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "epykos/algebra/egraph.hpp"
#include "epykos/algebra/error.hpp"
#include "epykos/algebra/recurrence.hpp"
#include "epykos/algebra/reduce.hpp"
#include "epykos/algebra/rule.hpp"
#include "epykos/algebra/term.hpp"
#include "epykos/ir/analysis.hpp"

namespace {

using epykos::algebra::Axiom;
using epykos::algebra::RecurrenceClass;
namespace alg = epykos::algebra;

TEST(AlgebraDesign, EveryAxiomHasADistinctName) {
  std::set<std::string> names;
  for (int i = 0; i < static_cast<int>(Axiom::Count_); ++i) {
    const std::string n = alg::to_string(static_cast<Axiom>(i));
    EXPECT_NE(n, "?") << "axiom " << i << " has no name in src/algebra/names.cpp";
    EXPECT_TRUE(names.insert(n).second) << "duplicate axiom name: " << n;
  }
  EXPECT_EQ(names.size(), static_cast<std::size_t>(Axiom::Count_));
}

TEST(AlgebraDesign, EveryRecurrenceClassHasADistinctName) {
  std::set<std::string> names;
  for (int i = 0; i < static_cast<int>(RecurrenceClass::Count_); ++i) {
    const std::string n = alg::to_string(static_cast<RecurrenceClass>(i));
    EXPECT_NE(n, "?");
    EXPECT_TRUE(names.insert(n).second) << "duplicate recurrence class name: " << n;
  }
}

TEST(AlgebraDesign, EveryEnumeratorIsNameable) {
  for (int i = 0; i < static_cast<int>(alg::OutputClass::Count_); ++i) {
    EXPECT_STRNE(alg::to_string(static_cast<alg::OutputClass>(i)), "?");
  }
  for (int i = 0; i <= static_cast<int>(alg::ExactnessHint::Inexact); ++i) {
    EXPECT_STRNE(alg::to_string(static_cast<alg::ExactnessHint>(i)), "?");
  }
  for (int i = 0; i <= static_cast<int>(alg::PathKind::AfterReduction); ++i) {
    EXPECT_STRNE(alg::to_string(static_cast<alg::PathKind>(i)), "?");
  }
  EXPECT_STREQ(alg::to_string(alg::Closure::Endpoints), "endpoints");
  EXPECT_STREQ(alg::to_string(alg::Closure::PerStep), "per_step");
  EXPECT_STREQ(alg::to_string(alg::CostMetric::WeightedNodes), "weighted_nodes");
}

// A statement about floating-point behaviour, not admissibility: since §4, the exact bucket
// carries no privilege. §3.
TEST(AlgebraDesign, AxiomExactnessBucketsArePartitioned) {
  for (int i = 0; i < static_cast<int>(Axiom::Count_); ++i) {
    const Axiom a = static_cast<Axiom>(i);
    EXPECT_FALSE(alg::axiom_is_unconditionally_exact(a) && alg::axiom_needs_side_condition(a))
        << alg::to_string(a) << " is claimed both unconditionally exact and conditional";
  }
}

// PRINCIPLES.md §4's central point: a rewrite may be admitted BECAUSE it is more accurate than the
// recording. Under M1-M4 these were inadmissible. If this list empties, §4 has been reverted.
TEST(AlgebraDesign, SomeAxiomsImproveAccuracy) {
  int improving = 0;
  for (int i = 0; i < static_cast<int>(Axiom::Count_); ++i) {
    if (alg::axiom_improves_accuracy(static_cast<Axiom>(i))) ++improving;
  }
  EXPECT_GE(improving, 3);
  EXPECT_TRUE(alg::axiom_improves_accuracy(Axiom::FmaContract));
  EXPECT_TRUE(alg::axiom_improves_accuracy(Axiom::MulRecipAsDiv));
  EXPECT_TRUE(alg::axiom_improves_accuracy(Axiom::FoldUntainted));
}

// The tape has no gathers, so the two IR-layer gather axioms are GONE rather than deferred. If one
// reappears here, the design has drifted back to the wrong layer. §0.1.
TEST(AlgebraDesign, NoGatherAxiomsAtTheTapeLayer) {
  for (int i = 0; i < static_cast<int>(Axiom::Count_); ++i) {
    const std::string n = alg::to_string(static_cast<Axiom>(i));
    EXPECT_EQ(n.find("gather"), std::string::npos) << n << " names a gather; the tape has none";
  }
}

// PRINCIPLES.md §2: "The op set does not grow for this." Two of the four recurrence classes
// therefore spell r^n as exp(n log r), which is why only the telescope gets an asymptotic win. §4.4.
TEST(AlgebraDesign, RecurrenceClassesNeedingExpLogAreNamed) {
  EXPECT_FALSE(alg::recurrence_needs_exp_log(RecurrenceClass::ConsecutiveRatioProduct));
  EXPECT_FALSE(alg::recurrence_needs_exp_log(RecurrenceClass::Arithmetic));
  EXPECT_TRUE(alg::recurrence_needs_exp_log(RecurrenceClass::Geometric));
  EXPECT_TRUE(alg::recurrence_needs_exp_log(RecurrenceClass::LinearConstantCoefficient));
}

// No default budget amounts to a contract. `no_rewrites()` is named for what it does;
// `naive_path_stage_a()` is the measured reference point (D72) and is NOT the default.
TEST(AlgebraDesign, NoDefaultBudgetAmountsToAContract) {
  const alg::ErrorBudget zero = alg::ErrorBudget::no_rewrites();
  for (int i = 0; i < static_cast<int>(alg::OutputClass::Count_); ++i) EXPECT_EQ(zero.relative[i], 0.0);

  // The oracle's measured numbers, so a re-measurement has to update this test too.
  const alg::ErrorBudget naive = alg::ErrorBudget::naive_path_stage_a();
  EXPECT_DOUBLE_EQ(naive.relative[static_cast<int>(alg::OutputClass::Valuation)], 1.6e-13);
  EXPECT_DOUBLE_EQ(naive.relative[static_cast<int>(alg::OutputClass::Sensitivity)], 2.1e-10);
  // Sensitivities have about three more decades of room than valuation: §4's "valuation is held
  // tight, sensitivities are allowed more" as a measurement rather than a policy.
  EXPECT_GT(naive.relative[static_cast<int>(alg::OutputClass::Sensitivity)],
            naive.relative[static_cast<int>(alg::OutputClass::Valuation)] * 100.0);

  const alg::Options opts;
  EXPECT_EQ(opts.budget.relative[0], 0.0);
  EXPECT_EQ(opts.propagator, nullptr);
  EXPECT_TRUE(opts.compact);
}

// Exactness is ADVISORY and defaults to Unknown everywhere. A default of `Exact` would let a
// rewrite be admitted for preserving bits, which is what §4 retired.
TEST(AlgebraDesign, ExactnessIsAdvisoryAndDefaultsToUnknown) {
  const alg::Rewrite r;
  EXPECT_EQ(r.exactness, alg::ExactnessHint::Unknown);
  const alg::EGraph::Applied a;
  EXPECT_EQ(a.exactness, alg::ExactnessHint::Unknown);
  const alg::ErrorTerm t;
  EXPECT_TRUE(t.exact);
  EXPECT_FALSE(t.across_select);
  alg::CandidateError e;
  EXPECT_TRUE(e.all_exact());
  e.contributions.push_back({3, alg::ErrorTerm{false, 1e-16, 0.0, false, false}, "alg.mul_assoc"});
  EXPECT_FALSE(e.all_exact());
}

// §4's slow/fast path test carries the NUMBER, not a boolean, because that number is the
// conditioning measurement. Exactness is observed, never assumed.
TEST(AlgebraDesign, PathAgreementCarriesTheNumber) {
  const alg::PathAgreement g;
  EXPECT_EQ(g.max_relative, 0.0);
  EXPECT_TRUE(g.within_tolerance);
  EXPECT_FALSE(g.exact);
  // The two ends of this stage are themselves a path pair: the tape before and after reduction.
  EXPECT_EQ(g.a, alg::PathKind::BeforeReduction);
  EXPECT_EQ(g.b, alg::PathKind::AfterReduction);
}

// The bounds that must not be relaxed silently. AC matching over Op::Sum has three independent
// reasons against it (egraph.hpp) and flipping it must be a deliberate edit.
TEST(AlgebraDesign, SearchBoundsAreOffByDefault) {
  const alg::Limits limits;
  EXPECT_FALSE(limits.ac_matching_on_sum);
  EXPECT_LE(limits.max_pattern_depth, 4);
  // Class-uniform saturation is what makes a 517,036-node tape tractable, so it is ON; a rule that
  // cannot take it opts out rather than the driver guessing.
  EXPECT_TRUE(limits.class_uniform);
}

// A proof that has not been established reports so, and one without (P4) offers only the per-step
// closure. Both are the conservative direction. §4.2.
TEST(AlgebraDesign, DefaultRecurrenceProofHoldsNothing) {
  alg::Proof p;
  EXPECT_FALSE(p.holds());
  EXPECT_FALSE(p.collapses_whole_chain());
  EXPECT_EQ(p.covered_steps, 0);
  p.klass = RecurrenceClass::ConsecutiveRatioProduct;
  p.step_shape_matched = true;
  EXPECT_TRUE(p.holds());
  EXPECT_FALSE(p.collapses_whole_chain());  // (P4) not discharged: PerStep only
  p.only_endpoint_read = true;
  EXPECT_TRUE(p.collapses_whole_chain());
}

// A rule does not get class-uniform application by default: the conservative direction costs only
// speed, while the wrong direction is silently unsound for a data-dependent side condition.
TEST(AlgebraDesign, ClassUniformityIsOptIn) {
  struct Probe : alg::Rule {
    const std::string& name() const noexcept override { return n_; }
    int pattern_depth() const noexcept override { return 1; }
    void search(const alg::View&, alg::MatchSink&) const override {}
    bool build(const alg::View&, const alg::Match&, alg::Rewrite&) const override { return false; }
    std::string n_ = "probe";
  } probe;
  EXPECT_FALSE(probe.applies_class_uniformly());
  EXPECT_EQ(probe.exactness_hint(), alg::ExactnessHint::Unknown);
  EXPECT_EQ(probe.opens_sites_per_kilonode(), 0u);
}

// A term is a node. No anchor domain, no row index, nothing else. §1.
TEST(AlgebraDesign, ATermIsANode) {
  static_assert(std::is_same_v<alg::TermRef, epykos::node_id>);
  const epykos::ir::Analysis a;
  EXPECT_TRUE(a.chains.empty());
  EXPECT_EQ(a.num_shape_classes, 0);
}

}  // namespace
