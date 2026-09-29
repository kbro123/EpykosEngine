// EpykosEngine — the stage itself: extraction and the one call site (P1; DESIGN ONLY; D73).
// `docs/TERM_REWRITING.md` §5 and §7.
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it.
//
// ---------------------------------------------------------------------------------------------
// Where this sits
// ---------------------------------------------------------------------------------------------
//
//     record -> standard_passes -> reduce()  <-- HERE -->  ir::infer -> plan -> execute
//
// One function, one call site, and nothing below it changes: inference, planning and execution
// receive a smaller tape and run exactly as they do now. That is the strongest property of the
// placement and it is what makes the package safe to land incrementally — with an empty rule set,
// `reduce` is the identity and every existing gate passes unchanged.
//
// Two call sites exist in the engine today and BOTH should get it (docs/TERM_REWRITING.md §8.2):
// the main recording path, and `solver::ResidualProgram`'s constructor
// (`src/solver/residual.cpp:73-77`), which does `standard_passes(slice_.tape)` then
// `ir::infer(slice_.tape)` with no optimiser in between. The second is where the calibration
// solve's ~48% of Stage A lives, and it is one line.
//
// ---------------------------------------------------------------------------------------------
// Two objectives, one of which is a constraint
// ---------------------------------------------------------------------------------------------
//
// PRINCIPLES.md §4 sets tolerances PER OUTPUT CLASS, and a tolerance is a constraint. So the
// honest formulation is not a Pareto search over (speed, accuracy) but
//
//     minimise predicted cost   subject to   predicted error <= budget(output class)
//
// a constrained single objective. What it gives up: a candidate much MORE accurate for slightly
// more cost is never preferred, because nothing asks for it. That is the right default — the owner
// sets budgets; the engine does not get to trade the owner's accuracy for speed on its own
// authority.
//
// The interesting direction is the other one, and it is why the oracle changes the SEARCH and not
// only the gate: a faster form is frequently also more accurate. `fma_contract` and
// `mul_recip_as_div` both reduce rounding; `fold_untainted` replaces a computed value with its
// exact constant; and the telescope removes a catastrophic cancellation AND is asymptotically
// cheaper. With the naive path's own error now measured at ~1.6e-13 (valuation) and ~2.1e-10
// (sensitivity) on Stage A (D72), the frontier may well be a single point in every case that
// matters — and if it usually is, all of the budget machinery beyond one scalar is
// over-engineering. §8.4 names the measurement.
//
// ---------------------------------------------------------------------------------------------
// What extraction prices
// ---------------------------------------------------------------------------------------------
//
// NOT the term. A rewrite's value is usually not local: collapsing a 90-step compounded coupon
// removes 89 multiplies and then makes ~88 of the 91 discount factors that fed it unreachable, and
// those are `exp` nodes. So extraction lowers each candidate to a tape, runs `cse` and `dce`, and
// prices THAT. Node count is the honest first metric here, not nanoseconds: at this layer there is
// no plan and no interpreter yet, so `optimise::estimate_program` cannot be called without
// inferring first. §5.5 argues that inferring each candidate is affordable precisely because the
// stage emits a handful of candidates, and that node count is a good enough proxy to rank them
// before paying for that.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "epykos/algebra/egraph.hpp"
#include "epykos/algebra/error.hpp"
#include "epykos/algebra/rule.hpp"
#include "epykos/ir/analysis.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::algebra {

// How a candidate is ranked once it fits the budget.
enum class CostMetric : std::uint8_t {
  // Reachable node count after cse+dce, weighted by a per-op table (an `exp` is not a `neg`).
  // Cheap, layer-appropriate, and enough to rank candidates that differ by orders of magnitude —
  // which, if the telescope works at all, is the case that matters.
  WeightedNodes = 0,
  // Run `ir::infer` on each candidate and price the Program with `optimise::estimate_program`.
  // Strictly better information and strictly more expensive (Stage A inference is ~1.0 s of a
  // 7.8 s record). Worth it when two candidates are close; wasteful when one is 250x smaller.
  InferAndEstimate = 1,
};

const char* to_string(CostMetric m) noexcept;

struct Options {
  // The accuracy constraint — the ONLY admission criterion, since PRINCIPLES.md §4 retired
  // bit-identity. There is deliberately no default that amounts to a contract: the numbers belong
  // in PROBLEM.md beside the outputs they govern, and a library default would quietly become the
  // contract the way bit-identity did (§0). `ErrorBudget::naive_path_stage_a()` is the measured
  // reference point, not a default.
  ErrorBudget budget = ErrorBudget::no_rewrites();
  OutputClassMap output_classes{};

  // Supplies |d(output)/d(node)|. May be null ONLY when `budget` is the zero budget; otherwise
  // `reduce` REFUSES rather than guessing, because a missing propagator silently reads as "no
  // error anywhere", which is the most dangerous possible default.
  const Propagator* propagator = nullptr;

  CostMetric metric = CostMetric::WeightedNodes;
  Limits limits{};

  // Re-run cse and dce on the winner before returning. On by default: it is how the win is
  // realised (egraph.hpp), not a tidy-up.
  bool compact = true;

  // A veto on the reduced tape, for a caller that needs an invariant preserved. nullptr: nothing
  // is rejected.
  std::function<bool(const Tape&)> reject;
};

struct Result {
  bool changed = false;
  std::size_t nodes_before = 0;
  std::size_t nodes_after = 0;

  // Per-rewrite breakdown, carried out rather than recomputed, so the oracle gate can report
  // "predicted 3.1e-13, measured 2.7e-13" and a divergence is visible.
  CandidateError error{};
  double predicted_relative[static_cast<int>(OutputClass::Count_)] = {0.0, 0.0, 0.0};

  // Rule names in application order, then the axiom or proof that licensed each. Long, and worth
  // it: a candidate nobody can explain is a candidate nobody should ship.
  std::vector<std::string> history;
  std::vector<std::string> justifications;

  Report saturation{};
  std::size_t candidates_considered = 0;
  std::size_t candidates_rejected_budget = 0;
  std::size_t candidates_rejected_guard = 0;

  // `PassResult`-shaped so a caller can compose it with the existing passes: remap[old] = new, or
  // invalid_node when the node was removed. Input and output ordinals are preserved, exactly as
  // `tape/passes.hpp` promises for every other pass.
  std::vector<node_id> remap;
};

// THE STAGE. Rewrites `tape` in place, as every other tape pass does, and returns what it did.
// With an empty `rules` it is the identity and returns `changed == false` — which is how this
// lands without touching a single existing gate.
Result reduce(Tape& tape, const std::vector<const Rule*>& rules, const Options& options = {});

// The same, when the caller already has an analysis (it needs one anyway for chain detection, and
// computing it twice would be silly).
Result reduce(Tape& tape, const ir::Analysis& analysis, const std::vector<const Rule*>& rules,
              const Options& options = {});

// Lower one chosen assignment (class -> e-node) back into a fresh Tape. Separated out because it
// is the part a round-trip test pins: lowering the graph's own initial assignment must reproduce
// the input tape EXACTLY. That is PRINCIPLES.md §4's structural tier — a graph identity, where
// "exact" costs nothing because no arithmetic happens — and it is the analogue of M1/P3's
// round-trip gate. It should land before any rule does.
Tape lower(const EGraph& graph, const std::vector<NodeId>& chosen);

}  // namespace epykos::algebra
