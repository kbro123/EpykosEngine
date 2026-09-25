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

struct TermExtractOptions {
  // The accuracy constraint. `ErrorBudget::exact_only()` reproduces the M1-M4 contract exactly:
  // only E0 rewrites are selectable and the result is bit-identical to the recording. Every
  // existing gate keeps its meaning under that setting, which is how this lands without
  // invalidating M1-M3.
  rewrite::ErrorBudget budget = rewrite::ErrorBudget::exact_only();
  rewrite::OutputClassMap output_classes{};

  // Supplies |d(output)/d(term)| for the composition in error_model.hpp. May be null ONLY when
  // `budget` is exact_only() (no error to propagate); otherwise extraction refuses rather than
  // guessing, because a missing propagator silently reads as "no error anywhere", which is the
  // most dangerous possible default.
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
// reproduce the input Program exactly, which is the term tier's analogue of M1/P3's round-trip
// identity and should land before any rule does.
ir::Program lower(const TermEGraph& graph, const std::vector<TermNodeId>& chosen);

}  // namespace epykos::optimise
