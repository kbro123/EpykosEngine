// EpykosEngine — the e-graph over TAPE nodes (P1; DESIGN ONLY; D73).
// `docs/TERM_REWRITING.md` §2 and §6.
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it. `optimise::EGraph` and
// `src/optimise/egraph.cpp` are untouched — see the note at the foot of this header for what
// becomes of them.
//
// ---------------------------------------------------------------------------------------------
// This is the ordinary thing, not the exotic one
// ---------------------------------------------------------------------------------------------
//
// An e-graph over a term DAG with hash-consing, congruence closure, deferred canonicalisation and
// a rebuild is textbook (egg, Willsey et al. 2021; D12: no external library, ours). The tape is
// a term DAG and is already hash-consed by `cse`. So the machinery here is standard and the
// interesting content of this package is everywhere else: what the rules are (rule.hpp), how a
// recurrence is proved (recurrence.hpp), how error composes (error.hpp), and what bounds the
// search (below).
//
// ---------------------------------------------------------------------------------------------
// The scale problem, stated honestly, because an earlier draft got it wrong
// ---------------------------------------------------------------------------------------------
//
// Measured (D44, M3-gate-1): the Stage A tape is **517,036 nodes** after the E0 passes, down from
// 27,459,283 recorded. That is a large seed for an e-graph. An earlier version of this design
// targeted `ir::Program` and argued tractability from the 517,036 -> 67-domain collapse. **That
// argument does not transfer and is retracted** (docs/TERM_REWRITING.md §0.1): at this layer the
// collapse has not happened yet. It is the thing we are running before.
//
// What rescues it is the same fact used differently. The tape is repetitive — that is WHY infer
// finds 67 domains — so the deep-hash partition (`ir::analysis.hpp`) puts those 517,036 nodes into
// a few hundred SHAPES. The driver saturates over one representative per shape class and applies
// the winner to every member, which a rule opts into with `Rule::applies_class_uniformly()`.
// Seeded that way the graph is ~10^3 nodes, not ~10^6.
//
// **Two things about that are unmeasured and I am not going to pretend otherwise.** The
// representative count is inferred from the domain count, not counted. And what the graph
// saturates TO is a different question from what it seeds at: associativity and distributivity
// over a ten-node term is the shape that grows fastest, and nobody has run it. §8.9 is the
// experiment. What IS structural, and is the claim worth relying on, is that the size is
// (number of shapes) x (per-shape saturation) and the second factor does not depend on the tape's
// 517,036 nodes.
//
// ---------------------------------------------------------------------------------------------
// Composing with cse and dce, which already run
// ---------------------------------------------------------------------------------------------
//
// `standard_passes` is cse, dce, fold_sum, affine_collapse, dce. The stage runs AFTER all of it,
// and the relationship is cooperative rather than competitive:
//
//   * `cse` is this graph's hash-consing, already done. Seeding from a post-cse tape means no two
//     seeded nodes are congruent, so the initial graph is already canonical.
//   * `dce` is how a rewrite's win is REALISED. Collapsing a chain does not itself delete the
//     nodes it replaced; it makes them unreachable. The stage therefore ends by re-running
//     `cse` then `dce`, and the measured shrink is the deliverable.
//   * `fold_sum` and `affine_collapse` have already normalised the shapes the rules match against,
//     which is a convenience: a rule can expect `Sum`/`Affine` rather than left-deep Add chains.
//     It is also a hazard — `affine_collapse` has rewritten `1 + f*w` into an `Affine` before the
//     recurrence classifier ever sees it, which is exactly why (P1) in recurrence.hpp says
//     "modulo the affine rearrangement the recorder and `affine_collapse` left behind".
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/algebra/error.hpp"
#include "epykos/algebra/rule.hpp"
#include "epykos/algebra/term.hpp"
#include "epykos/ir/analysis.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::algebra {

using NodeId = std::int64_t;   // an e-node's id, immutable for its whole life
inline constexpr NodeId invalid_enode = -1;

struct Limits {
  int max_rounds = 12;
  std::size_t max_nodes = 200000;
  std::size_t max_nodes_per_shape = 4000;

  // Saturate over one representative per shape class and apply the winner to every member. On by
  // default: it is what makes a 517,036-node tape tractable, and a rule that cannot take it says
  // so with `applies_class_uniformly() == false` and is matched against every node instead.
  bool class_uniform = true;

  // ---- the bound that matters most -----------------------------------------------------------
  // ASSOCIATIVE-COMMUTATIVE MATCHING ON Op::Sum AND Op::Affine IS OFF, AND MUST STAY OFF. Three
  // independent reasons (docs/TERM_REWRITING.md §3.4 and §6.1):
  //  (i)   Sum is a FIXED-ARITY LEFT FOLD in operand order (`ir/program.hpp`'s evaluation
  //        contract; `fold_sum` keeps the recorded order deliberately). Reordering its members
  //        changes the rounding by an amount NOTHING BOUNDS — a sum of mixed magnitudes can lose
  //        everything to cancellation under one order and nothing under another. PRINCIPLES.md
  //        §4's retirement of bit-identity does not relax this: the objection was never "it
  //        changes bits", it is that the error is unbounded and the family infinite, and a
  //        characterised-error contract refuses that more firmly, not less.
  //  (ii)  D61 canonicalised a COMMUTATIVE STEP's two operands and deliberately did NOT
  //        canonicalise a Sum's member order. AC matching would silently reverse that decision.
  //  (iii) Stage A's leg sums have segments of hundreds of members. n! orderings admit no bound.
  // The field exists so the decision is visible and a future package must change a default rather
  // than discover an absence.
  bool ac_matching_on_sum = false;

  // Patterns deeper than this are refused at registration. A constant-factor bound on the work per
  // candidate root (branching <= 3, depth <= 4), not an asymptotic one, and stated that way.
  int max_pattern_depth = 4;

  // egg-style banning with exponential backoff. A rule adding more than `ban_threshold` nodes in a
  // round is banned for `ban_rounds`, doubling on each re-offence. **This is the mechanism D65's
  // R2 needed and did not have**: at 10,111 sites it would have been banned in round 1 with a
  // logged reason instead of running 700 s and blowing the node bound.
  std::size_t ban_threshold = 2000;
  int ban_rounds = 2;
};

struct LogEntry {
  int round = 0;
  std::string rule;
  std::size_t matches = 0;
  std::size_t nodes_added = 0;
  std::size_t memoised = 0;   // (e-node, rule, binding) applications already applied
  std::size_t declined = 0;   // build() returned false: a side condition failed
  bool banned = false;
  std::string note;           // the bound's name when cut
};

struct Report {
  int rounds_run = 0;
  std::size_t total_nodes = 0;
  std::size_t total_classes = 0;
  std::size_t shapes_saturated = 0;   // representatives actually matched, under class_uniform
  bool reached_fixpoint = false;
  bool bound_hit = false;
  std::string bound_reason;
  std::vector<LogEntry> log;
};

class EGraph : public View {
 public:
  // Seeds from `tape`, using `analysis` for the shape partition and the chain list. Does not copy
  // the tape and does not modify it.
  EGraph(const Tape& tape, const ir::Analysis& analysis);
  ~EGraph() override;
  EGraph(const EGraph&) = delete;
  EGraph& operator=(const EGraph&) = delete;

  // ---- View ------------------------------------------------------------------------------------
  const Tape& tape() const noexcept override;
  std::int64_t num_classes() const noexcept override;
  std::vector<NodeView> nodes_of(ClassId c) const override;
  ClassId class_of(node_id n) const noexcept override;
  bool untainted(ClassId c) const override;
  bool constant_value(ClassId c, double& out) const override;
  bool provably_positive(ClassId c) const override;
  bool provably_nonzero(ClassId c) const override;
  bool provably_finite(ClassId c) const override;
  bool provably_not_negative_zero(ClassId c) const override;
  std::int32_t shape_class_of(node_id n) const noexcept override;
  std::int32_t shape_class_size(std::int32_t shape_class) const noexcept override;

  // ---- saturation --------------------------------------------------------------------------------
  // Deferred canonicalisation: a whole round's matches are queued, then one rebuild hash-conses and
  // unions them all. Repeatable; a later call resumes.
  Report saturate(const std::vector<const Rule*>& rules, const Limits& limits = {});

  // Interns one rule's replacement and unions it with the site's class. The only mutator a rule's
  // output reaches, and the DRIVER calls it, never the rule (rule.hpp). Returns false when the
  // replacement is rejected (a node budget hit, a malformed Expr) — a logged event, never silent.
  bool apply(const Rewrite& rewrite);

  // ---- invariants (a gate test calls these directly) ---------------------------------------------
  bool congruent();

  struct Applied {
    ClassId site = invalid_class;
    node_id representative = invalid_node;
    std::int32_t shape_class = -1;
    std::int32_t applied_to_members = 0;   // 1 unless class-uniform application expanded it
    ExactnessHint exactness = ExactnessHint::Unknown;
    ErrorTerm error{};
    std::string rule;
    std::string justification;
  };
  const std::vector<Applied>& applied() const noexcept;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace epykos::algebra

// ------------------------------------------------------------------------------------------------
// What becomes of `optimise::EGraph` (docs/TERM_REWRITING.md §7)
// ------------------------------------------------------------------------------------------------
//
// It is NOT rebuilt, and the whole-program-e-node problem D62 recorded is not solved — it is
// bypassed, because algebra no longer happens at that layer and layout rewrites do not need a
// saturating search to find. The recommendation is that the program tier is RETIRED AS A SEARCH
// and its plan tier becomes a deterministic compilation pass after this stage and after inference:
// the interpreter needs a plan regardless, D63 records that the joint search "passes BY IDENTITY —
// the extracted candidate is the default plan's own execution", and D68 prices a perfect
// plan-level cost model at ~0.08% of Stage A's wall clock. Deleting the tier also deletes
// `RefirePolicy::PipelineOrderedPlans` and the completeness it gave up.
//
// The seven layout rules are unaffected in what they DO; only the driver above them changes.
