// EpykosEngine — extraction from the term e-graph, under two objectives (P1/term-rewriting
// DESIGN ONLY; D73; docs/TERM_REWRITING.md §5).
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it. `optimise::extract` (extract.hpp) is
// untouched and keeps extracting from the program/plan tiers.
//
// ---------------------------------------------------------------------------------------------
// Two objectives, one of which is a constraint
// ---------------------------------------------------------------------------------------------
//
// PRINCIPLES.md §4 sets tolerances PER OUTPUT CLASS, and a tolerance is a constraint, not
// something to minimise. So the honest formulation is not a Pareto search over (speed, accuracy)
// but:
//
//     minimise predicted cost   subject to   predicted error <= budget(output class)
//
// which is a constrained single objective and which the existing bottom-up DP already does, once
// per error tier. `docs/TERM_REWRITING.md` §5.3 says what that gives up (a candidate that is much
// more accurate for slightly more cost is never preferred, because nothing asks for it) and why
// that is the right default: the owner sets budgets; the engine does not get to trade the owner's
// accuracy for speed on its own authority.
//
// The interesting direction is the other one, and it is why §4's oracle changes the search rather
// than just the gate: a FASTER form is frequently also MORE accurate. `fma_contraction` and
// `mul(a, recip(b)) -> div(a, b)` both reduce rounding; the telescope replaces a catastrophic
// `(DF_s/DF_e - 1)` cancellation with a single exact-in-R ratio and is about 250x cheaper. Under
// the M1-M4 bit-identity contract all three were inadmissible. Under this one the Pareto frontier
// is often a single point, and if that is USUALLY true the two-objective machinery is
// over-engineering. §8.4 names the (cheap) measurement that would settle it.
//
// ---------------------------------------------------------------------------------------------
// Why extraction prices the WHOLE PROGRAM, not the term
// ---------------------------------------------------------------------------------------------
//
// A term rewrite's value is usually not local. Telescoping a 90-step compounded coupon removes
// 89 multiplies from the scan domain — and then kills about 88 of the 91 discount factors that
// fed it, which are rows of the `exp(mul(neg(@0),$0))` domain, 16,917 rows on Stage A, and
// `exp` was 50%+ of the M1 book's B=64 time. That win is a DEAD-CODE CONSEQUENCE downstream of
// the rewrite, in a different domain, and a local cost delta cannot see any of it.
//
// So extraction lowers each candidate to a whole `ir::Program`, runs DCE, and prices THAT with
// `optimise::estimate_program`. That is affordable precisely because the term tier emits a
// handful of candidates (one per error tier), not one per match — the inversion of the property
// that killed the whole-Program e-graph.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/optimise/term_egraph.hpp"
#include "epykos/rewrite/error_model.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::optimise {

// --------------------------------------------------------------------------------------------
// WHERE PLANNING HAPPENS: an OPEN QUESTION, designed both ways (docs/TERM_REWRITING.md §7.4)
// --------------------------------------------------------------------------------------------
//
// The layout/planner rules (r6 plus the five `planner.*` decisions) can either stay inside the
// search as the PLAN TIER, or leave the search entirely and become a deterministic compilation
// pass run AFTER extraction. The owner has not decided. Both are expressible against this
// interface and the difference is one enum plus which of two entry points a caller uses, which is
// itself an argument that the interface is at the right altitude.
//
// This design's RECOMMENDATION is `PostExtractionPass`, with the reasoning and the residual risk
// in §7.4. It is a recommendation, not an assumption: `InSearch` is implementable unchanged and
// `plan_after_extraction` simply goes unused.
enum class PlanningMode : std::uint8_t {
  // D49/D62's shape. Plans are e-graph candidates: extraction scores (program, plan) pairs
  // jointly, so a plan can pay for a program that is worse on its own. Costs D62's 2^k plan-tier
  // lattice (105,977 plan nodes / 10.5 GiB on 60 trades) and needs
  // `RefirePolicy::PipelineOrderedPlans` to stay bounded, with the completeness that policy gives
  // up. `TermExtractResult::plan` is then chosen BY the search.
  InSearch = 0,
  // Extraction ranks PROGRAMS only, under the default plan; the winner is then planned once, by
  // a deterministic pass. The search space loses a dimension and the plan tier disappears.
  // `TermExtractResult::plan` is then produced by `plan_after_extraction`, not searched.
  PostExtractionPass = 1,
};

const char* to_string(PlanningMode m) noexcept;

struct TermExtractOptions {
  // The accuracy constraint — the ONLY admission criterion, since PRINCIPLES.md §4 retired
  // bit-identity (2026-09-25). There is deliberately no default value that amounts to a contract:
  // the numbers belong in PROBLEM.md beside the outputs they govern, and a library default would
  // quietly become the contract the way bit-identity did. `ErrorBudget::no_rewrites()` is the
  // zero budget, useful for bisection and tests, and is NOT "the safe setting" — it rejects
  // rewrites that are strictly MORE accurate than what they replace.
  rewrite::ErrorBudget budget = rewrite::ErrorBudget::no_rewrites();
  rewrite::OutputClassMap output_classes{};

  // See PlanningMode. The default is this design's recommendation, §7.4.
  PlanningMode planning = PlanningMode::PostExtractionPass;

  // Supplies |d(output)/d(term)| for the composition in error_model.hpp. May be null ONLY when
  // `budget` is the zero budget (nothing inexact can be admitted, so there is nothing to
  // propagate); otherwise extraction REFUSES rather than guessing, because a missing propagator
  // silently reads as "no error anywhere", which is the most dangerous possible default and is
  // the shape of the assumption PRINCIPLES.md §0 exists to prevent.
  const rewrite::ErrorPropagator* propagator = nullptr;

  // Pricing parameters, forwarded to estimate_program. `lane_tile` carries D63's warning
  // unchanged: it must be `min(Options::lane_tile, Options::max_batch)`, the width the
  // interpreter you intend to build would actually PLAN with.
  int B = 1;
  int tile = 256;
  int lane_tile = 8;

  // Run DCE on each lowered candidate before pricing. On by default and it is not cosmetic: see
  // this header's second section. Off only for a test that wants to see the un-DCE'd cost.
  bool dce_before_pricing = true;

  // The same program-level veto extract.hpp already has (D54's cross-stage-sharing guard). Kept
  // identical so a caller's existing guard works unchanged.
  std::function<bool(const ir::Program&)> reject;
};

struct TermExtractResult {
  bool found = false;
  ir::Program program;                    // the lowered, DCE'd winner
  // Under PlanningMode::InSearch, the plan the search chose alongside `program`. Under
  // PostExtractionPass, what `plan_after_extraction` produced for it. Either way the caller reads
  // one field and does not need to know which mode ran — that symmetry is the reason the open
  // question can be deferred without stalling anything downstream.
  ir::PlanAnnotations plan;
  PlanningMode planning = PlanningMode::PostExtractionPass;
  double estimated_ns = 0.0;

  // What the winner's error is predicted to be, per output class, and the per-rewrite breakdown
  // it was composed from. Carried out of extraction rather than recomputed, so that the oracle
  // gate can report "predicted 3.1e-13, measured 2.7e-13" and a divergence is visible.
  rewrite::CandidateError error{};
  double predicted_relative[static_cast<int>(rewrite::OutputClass::Count_)] = {0.0, 0.0, 0.0};

  // Rule names in application order, then the axiom or proof that licensed each. Long, and worth
  // it: a candidate nobody can explain is a candidate nobody should ship.
  std::vector<std::string> history;
  std::vector<std::string> justifications;

  std::size_t candidates_considered = 0;
  std::size_t candidates_rejected_budget = 0;
  std::size_t candidates_rejected_guard = 0;
};

// Bottom-up over the term graph, then lower, DCE and price. Deterministic in the same sense
// extract.hpp already is: candidates visited in ascending class id, ties keeping the first seen.
TermExtractResult extract_terms(const TermEGraph& graph, const CostModel& model,
                                const TermExtractOptions& options = {});

// Lower one chosen assignment (class -> e-node) back to an `ir::Program`. Separated out because
// it is the part a round-trip test pins: lowering the graph's own initial assignment must
// reproduce the input Program EXACTLY — a graph identity, PRINCIPLES.md §4's structural tier,
// where "exact" costs nothing because no arithmetic happens. That is the term tier's analogue of
// M1/P3's round-trip identity and should land before any rule does.
ir::Program lower(const TermEGraph& graph, const std::vector<TermNodeId>& chosen);

// --------------------------------------------------------------------------------------------
// Planning as a post-extraction pass (PlanningMode::PostExtractionPass)
// --------------------------------------------------------------------------------------------
//
// One deterministic function of (program, execution parameters) -> plan. Not a rule, not a
// candidate, not searched: the interpreter needs a plan whatever the optimiser decides, and this
// is that. It is the existing `rewrite::planner::default_plan` (D63) with a wider remit — the
// five planner decisions plus r6's materialisation boundaries — and it may hill-climb internally
// on the cost model, which is a LOCAL search over one program rather than a dimension of the
// global one.
//
// Why this is not a loss of generality, and where it IS one, is §7.4's argument. The short form:
// the plan is a function of the program, the same program always gets the same plan, and the only
// thing given up is a plan paying for a program that is worse without it. Nothing in the record
// shows that pairing ever mattered — D63's own rediscovery passes BY IDENTITY, the extracted
// candidate being the default plan's own execution.
struct PlanningParams {
  int B = 1;
  int tile = 256;
  // D63's warning, unchanged: `min(Options::lane_tile, Options::max_batch)`, the width the
  // interpreter will actually PLAN with, not the nominal option.
  int lane_tile = 8;
  // Let the pass hill-climb on the cost model instead of taking the default plan as-is. Off by
  // default, because D68 measured the whole plan stage at 1.027x-1.042x on Stage A and a PERFECT
  // plan-level cost model at ~0.08% of its wall clock: the honest default is "do not spend search
  // time here", and the flag exists so that a workload with real layout structure (PRINCIPLES.md
  // §7's 1.73x-1.88x) can turn it on.
  bool hill_climb = false;
  int max_hill_climb_rounds = 3;
};

ir::PlanAnnotations plan_after_extraction(const ir::Program& program, const CostModel& model,
                                          const PlanningParams& params = {});

}  // namespace epykos::optimise
