// EpykosEngine — the term-rewriting DESIGN headers compile, and their enumerations are complete
// (P1, D73; docs/TERM_REWRITING.md).
//
// This package ships an INTERFACE, not an implementation, so there is no behaviour to test. What
// there IS to gate, and what this file gates:
//
//   1. Every new header compiles on its own, under every preset and both compilers. D46's lesson
//      (GCC contracts in C++ even in ISO mode; a header-only template is not exempt from a
//      toolchain difference) applies to headers nobody has instantiated yet as much as to any
//      other, and a header that has never been through GCC 13 is a header that does not compile.
//   2. Every `Axiom` and every `RecurrenceClass` has a name. The identity table in
//      `docs/TERM_REWRITING.md` §3 and the `Axiom` enum are two statements of the same list, and
//      the cheapest guard against them drifting is that adding an enumerator without a name
//      fails here.
//   3. The classification predicates over `Axiom` agree with the design document's three-way
//      split, and no axiom is in two buckets at once. That is the part a reader is most likely to
//      get wrong when adding one.
//   4. Nothing in the interface lets a rewrite be admitted BECAUSE it preserves bits.
//      PRINCIPLES.md §4 retired that on 2026-09-25 and these tests are what stop it creeping back
//      in as a default.
//
// Nothing here constructs a `TermEGraph` or calls any of the declared-but-undefined functions:
// they are undefined on purpose and will stay that way until the interface has been reviewed.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "epykos/optimise/term_egraph.hpp"
#include "epykos/optimise/term_extract.hpp"
#include "epykos/rewrite/error_model.hpp"
#include "epykos/rewrite/recurrence.hpp"
#include "epykos/rewrite/term.hpp"
#include "epykos/rewrite/term_rule.hpp"

namespace {

using epykos::rewrite::Axiom;
using epykos::rewrite::RecurrenceClass;

TEST(TermRewritingDesign, EveryAxiomHasADistinctName) {
  std::set<std::string> names;
  for (int i = 0; i < static_cast<int>(Axiom::Count_); ++i) {
    const std::string n = epykos::rewrite::to_string(static_cast<Axiom>(i));
    EXPECT_NE(n, "?") << "axiom " << i << " has no name in src/rewrite/term_names.cpp";
    EXPECT_TRUE(names.insert(n).second) << "duplicate axiom name: " << n;
  }
  EXPECT_EQ(names.size(), static_cast<std::size_t>(Axiom::Count_));
}

TEST(TermRewritingDesign, EveryRecurrenceClassHasADistinctName) {
  std::set<std::string> names;
  for (int i = 0; i < static_cast<int>(RecurrenceClass::Count_); ++i) {
    const std::string n = epykos::rewrite::to_string(static_cast<RecurrenceClass>(i));
    EXPECT_NE(n, "?");
    EXPECT_TRUE(names.insert(n).second) << "duplicate recurrence class name: " << n;
  }
}

TEST(TermRewritingDesign, EveryOutputClassHasAName) {
  for (int i = 0; i < static_cast<int>(epykos::rewrite::OutputClass::Count_); ++i) {
    EXPECT_STRNE(epykos::rewrite::to_string(static_cast<epykos::rewrite::OutputClass>(i)), "?");
  }
}

// An axiom is exact unconditionally, or exact under a discharged side condition, or neither (a
// characterised error). Never two of those. This is a statement about floating-point behaviour,
// not about admissibility — since §4, the exact bucket carries no privilege. §3.
TEST(TermRewritingDesign, AxiomExactnessBucketsArePartitioned) {
  for (int i = 0; i < static_cast<int>(Axiom::Count_); ++i) {
    const Axiom a = static_cast<Axiom>(i);
    const bool unconditional = epykos::rewrite::axiom_is_unconditionally_exact(a);
    const bool conditional = epykos::rewrite::axiom_needs_side_condition(a);
    EXPECT_FALSE(unconditional && conditional)
        << epykos::rewrite::to_string(a) << " is claimed both unconditionally exact and conditional";
  }
}

// PRINCIPLES.md §4's central point: a rewrite may be admitted BECAUSE it is more accurate than the
// recording. Under M1-M4 these two were inadmissible. If this list ever empties, the §4 contract
// has been quietly reverted.
TEST(TermRewritingDesign, SomeAxiomsImproveAccuracy) {
  int improving = 0;
  for (int i = 0; i < static_cast<int>(Axiom::Count_); ++i) {
    if (epykos::rewrite::axiom_improves_accuracy(static_cast<Axiom>(i))) ++improving;
  }
  EXPECT_GE(improving, 2);
  EXPECT_TRUE(epykos::rewrite::axiom_improves_accuracy(Axiom::FmaContract));
  EXPECT_TRUE(epykos::rewrite::axiom_improves_accuracy(Axiom::MulRecipAsDiv));
}

// PRINCIPLES.md §2: "The op set does not grow for this." Two of the four recurrence classes
// therefore have to spell r^n as exp(n log r), which is why only the telescope gets an asymptotic
// win. docs/TERM_REWRITING.md §4.4 — the arithmetic behind that claim is in the document, and
// this pins the classification it rests on.
TEST(TermRewritingDesign, RecurrenceClassesNeedingExpLogAreNamed) {
  EXPECT_FALSE(epykos::rewrite::recurrence_needs_exp_log(RecurrenceClass::ConsecutiveRatioProduct));
  EXPECT_FALSE(epykos::rewrite::recurrence_needs_exp_log(RecurrenceClass::Arithmetic));
  EXPECT_TRUE(epykos::rewrite::recurrence_needs_exp_log(RecurrenceClass::Geometric));
  EXPECT_TRUE(epykos::rewrite::recurrence_needs_exp_log(RecurrenceClass::LinearConstantCoefficient));
}

// PRINCIPLES.md §4 (amended 2026-09-25) retired bit-identity as a contract, so the library ships
// NO default budget that amounts to one. `no_rewrites()` is the zero budget: named for what it
// does, documented as a bisection tool, and explicitly not "the safe setting".
TEST(TermRewritingDesign, ZeroBudgetIsNamedForWhatItDoesNotForWhatItPreserves) {
  const epykos::rewrite::ErrorBudget b = epykos::rewrite::ErrorBudget::no_rewrites();
  for (int i = 0; i < static_cast<int>(epykos::rewrite::OutputClass::Count_); ++i) {
    EXPECT_EQ(b.relative[i], 0.0);
  }
  const epykos::optimise::TermExtractOptions opts;
  EXPECT_EQ(opts.budget.relative[0], 0.0);
  EXPECT_EQ(opts.propagator, nullptr);
  EXPECT_TRUE(opts.dce_before_pricing);
}

// Exactness is ADVISORY. A rule that has not characterised itself says so, and `Unknown` is the
// default at every level — rule, rewrite and recorded application. If any of these ever defaults
// to `Exact`, a rewrite could be admitted for preserving bits, which is exactly what §4 retired.
TEST(TermRewritingDesign, ExactnessIsAdvisoryAndDefaultsToUnknown) {
  const epykos::rewrite::TermRewrite r;
  EXPECT_EQ(r.exactness, epykos::rewrite::ExactnessHint::Unknown);
  const epykos::optimise::TermEGraph::AppliedRewrite a;
  EXPECT_EQ(a.exactness, epykos::rewrite::ExactnessHint::Unknown);
  for (int i = 0; i <= static_cast<int>(epykos::rewrite::ExactnessHint::Inexact); ++i) {
    EXPECT_STRNE(epykos::rewrite::to_string(static_cast<epykos::rewrite::ExactnessHint>(i)), "?");
  }
}

// §4's slow/fast path test. Every path kind is nameable, and an agreement result carries the
// NUMBER, not just a boolean, because the number is the conditioning measurement §4 wants.
TEST(TermRewritingDesign, PathAgreementCarriesTheNumberNotJustABoolean) {
  for (int i = 0; i <= static_cast<int>(epykos::rewrite::PathKind::Oracle); ++i) {
    EXPECT_STRNE(epykos::rewrite::to_string(static_cast<epykos::rewrite::PathKind>(i)), "?");
  }
  const epykos::rewrite::PathAgreement g;
  EXPECT_EQ(g.max_relative, 0.0);
  EXPECT_TRUE(g.within_tolerance);
  EXPECT_FALSE(g.exact);  // exactness is observed, never assumed
}

// The planning question is open (§7.4). Both modes are expressible, both are named, and the
// result carries which one ran so a caller reads one `plan` field either way.
TEST(TermRewritingDesign, BothPlanningModesAreExpressible) {
  EXPECT_STREQ(epykos::optimise::to_string(epykos::optimise::PlanningMode::InSearch), "in_search");
  EXPECT_STREQ(epykos::optimise::to_string(epykos::optimise::PlanningMode::PostExtractionPass),
               "post_extraction_pass");
  const epykos::optimise::TermExtractOptions opts;
  EXPECT_EQ(opts.planning, epykos::optimise::PlanningMode::PostExtractionPass);  // the recommendation
  const epykos::optimise::TermExtractResult res;
  EXPECT_EQ(res.planning, epykos::optimise::PlanningMode::PostExtractionPass);
  // D68's measurement is why hill-climbing is off by default: a PERFECT plan-level cost model is
  // worth ~0.08% of Stage A's wall clock.
  const epykos::optimise::PlanningParams params;
  EXPECT_FALSE(params.hill_climb);
}

// The bound that must not be relaxed silently: AC matching over Op::Sum. term_egraph.hpp gives
// three independent reasons; this makes flipping the default a visible, deliberate edit.
TEST(TermRewritingDesign, AcMatchingOnSumIsOffByDefault) {
  const epykos::optimise::TermSaturationLimits limits;
  EXPECT_FALSE(limits.ac_matching_on_sum);
  EXPECT_LE(limits.max_pattern_depth, 4);
  EXPECT_TRUE(limits.cross_domain_single_reader_only);
}

// A default-constructed ErrorTerm is exact and costs nothing: an E0 rule that forgets to fill it
// in is correct by default rather than silently claiming an error of zero it does not have.
TEST(TermRewritingDesign, DefaultErrorTermIsExact) {
  const epykos::rewrite::ErrorTerm t;
  EXPECT_TRUE(t.exact);
  EXPECT_EQ(t.relative_delta, 0.0);
  EXPECT_FALSE(t.across_select);

  epykos::rewrite::CandidateError e;
  EXPECT_TRUE(e.all_exact());
  e.contributions.push_back({0, 1, epykos::rewrite::ErrorTerm{false, 1e-16, 0.0, false, false}, "alg.mul_assoc"});
  EXPECT_FALSE(e.all_exact());
}

// A TermRef names one sub-DAG: (domain, step). Row indices appear nowhere, because a term rewrite
// applies to every row of its anchor domain and there is no row-subset form (term.hpp point 2).
TEST(TermRewritingDesign, TermRefNamesADomainAndAStepAndNothingElse) {
  const epykos::rewrite::TermRef unset;
  EXPECT_FALSE(unset.valid());
  const epykos::rewrite::TermRef r{7, 3};
  EXPECT_TRUE(r.valid());
  EXPECT_EQ(r.domain, 7);
  EXPECT_EQ(r.step, 3);
  EXPECT_EQ(sizeof(epykos::rewrite::TermRef), 2 * sizeof(std::int32_t));
}

// A proof that has not been established reports so, and a proof without (P4) offers only the
// per-row closure. Both are the conservative direction.
TEST(TermRewritingDesign, DefaultRecurrenceProofHoldsNothing) {
  epykos::rewrite::RecurrenceProof p;
  EXPECT_FALSE(p.holds());
  EXPECT_FALSE(p.collapses_rows());
  p.klass = RecurrenceClass::ConsecutiveRatioProduct;
  p.step_shape_matched = true;
  EXPECT_TRUE(p.holds());
  EXPECT_FALSE(p.collapses_rows());  // (P4) not discharged: PerRow only
  p.only_chain_final_rows_read = true;
  EXPECT_TRUE(p.collapses_rows());
}

}  // namespace
