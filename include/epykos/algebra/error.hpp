// EpykosEngine — how a rewrite's error is stated, and how errors COMPOSE (P1; DESIGN ONLY; D73;
// docs/PRINCIPLES.md §4 as amended 2026-09-25; docs/TERM_REWRITING.md §5).
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it.
//
// ---------------------------------------------------------------------------------------------
// Bit-identity is RETIRED as a contract, so this file is the only admission criterion there is
// ---------------------------------------------------------------------------------------------
//
// PRINCIPLES.md §4 leaves two tiers and a test. STRUCTURAL is exact because it is a graph identity
// and rounding does not arise. MATHEMATICAL is characterised error against the oracle, and it now
// covers EVERY EXECUTION PATH too, so the interpreter, the catalogue kernels and the adjoint are
// implementations of the recorded maths like any rewrite and are judged the same way.
//
// So: **a rewrite is never admissible BECAUSE it preserves bits.** It is admissible when its
// composed error fits the output class's budget. `ExactnessHint` survives only as a bug detector.
//
// ---------------------------------------------------------------------------------------------
// The oracle is real as of 2026-09-25, and these are its numbers
// ---------------------------------------------------------------------------------------------
//
// `epykos::Wide` (D72, branch `p0/oracle`) is an in-repo double-double at 106 significand bits,
// 2^53 finer than a double ulp, and the templated maths instantiates at it unchanged (D3's
// dividend). The NAIVE PATH'S OWN ERROR against it is now measured rather than assumed:
//
//     Stage A valuation      ~1.6e-13   (leg PV; trade PV 7.9e-14, aggregates 1.5e-13)
//     Stage A sensitivity    ~2.1e-10   (leg PV; trade PV 5.2e-11, aggregates 1.2e-13)
//
// Three things follow, and they are why this header is written against numbers instead of
// hypotheticals (docs/TERM_REWRITING.md §5.4):
//
//   1. **The headroom is known.** A rewrite whose composed error lands at or below the naive
//      path's own is free in any practical sense — it does not move the answer by more than the
//      answer was already moving. That is about 1.6e-13 for valuation.
//   2. **Sensitivities have roughly three more decades of room** (2.1e-10 against 1.6e-13), which
//      is the empirical content of §4's "valuation is held tight; sensitivities are allowed more".
//      It was a policy; it is now a measurement.
//   3. **The 2.1e-10 is itself a conditioning signal.** Sensitivities are 656x worse than
//      valuation on Stage A. A rewrite that removes a cancellation — the telescope removes
//      `(DF_s/DF_e - 1)`, where the ratio is within ~1e-4 of 1 and the subtraction discards about
//      thirteen digits — should MOVE THAT NUMBER DOWN. If it does not, the model is wrong.
//
// ---------------------------------------------------------------------------------------------
// The problem: a per-op ulp count does not compose
// ---------------------------------------------------------------------------------------------
//
// M1-M4 classified a rewrite E0 (bit-identical) or E1 ("<= 1 ulp per op", D26/D30). Two separate
// reasons that is useless for composing several rewrites:
//
//   (a) AN ULP IS NOT A SCALE. It is the quantum of a representable neighbourhood, so "1 ulp" at
//       1e-300 and at 1e+300 are incomparable. Adding them is meaningless.
//   (b) EVEN RELATIVE ERROR DOES NOT COMPOSE THROUGH A CHAIN. Multiplicative steps compose
//       benignly; subtraction of nearby quantities amplifies its operands' relative error by
//       (|a|+|b|)/|a-b|, unbounded. Stage A's own recorded forward rate is exactly that shape, and
//       the ~1e4 amplification it implies is visible in the 656x above.
//
// ---------------------------------------------------------------------------------------------
// What DOES compose: first-order propagation by the adjoint the engine already has
// ---------------------------------------------------------------------------------------------
//
// Model each rewrite as an ABSOLUTE perturbation injected at the node it replaces:
//
//     d(output o)  =  SUM over rewritten nodes t  of  |d(o)/d(t)| * |dt|
//
// A linear functional, so it composes; and nearly free HERE because `d(o)/d(t)` is what the
// mechanical adjoint already computes (M2, D31), in one reverse pass per output seed. At the TAPE
// layer this is more direct than it would have been over `ir::Program`: the adjoint is defined
// over the same node ids the rewriter names.
//
//     THE ADJOINT-WEIGHTED NUMBER IS AN ESTIMATE, NOT A BOUND. It is a linearisation, and it is
//     the SEARCH HEURISTIC. The GATE is the oracle above, run on the ONE extracted candidate.
//
// Two known invalidities, handled rather than hoped away: across a `Select` (the derivative w.r.t.
// the predicate is zero, so a rewrite that could flip the branch has first-order effect zero and a
// real effect of the arm difference), and where the value can vanish (relative error undefined).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epykos/tape/tape.hpp"

namespace epykos::algebra {

// The LOCAL error one rewrite introduces, against PRINCIPLES.md §4's ground truth — the recorded
// expression at infinite precision, NOT the naive `double` evaluation of it, which is merely one
// realisation and now has its own measured error (see the header).
//
// The sign convention matters and is the point of §4: `fma_contract` and `mul(a, recip(b)) ->
// div(a, b)` both REDUCE error, and under the M1-M4 contract both were failures. Here they carry a
// negative `relative_delta` and extraction may prefer them.
struct ErrorTerm {
  bool exact = true;             // identically zero: no propagation needed
  double relative_delta = 0.0;   // signed; negative means CLOSER to the infinite-precision value
  double absolute_delta = 0.0;   // for nodes whose value may be zero
  bool bound_is_proved = false;  // false: a modelled estimate, not a proof
  bool across_select = false;    // first-order propagation invalid here; bound by the arm difference

  static ErrorTerm zero() noexcept { return ErrorTerm{}; }
};

// Advisory only. Two jobs, both tests rather than gates (PRINCIPLES.md §4: "a test of convenience,
// never a constraint"): a FREE ASSERTION where exactness costs nothing to check — it is what
// caught D61's GCC operand-order divergence — and a SHORTCUT past the propagator when every
// contribution is exact. It must never decide admissibility.
enum class ExactnessHint : std::uint8_t {
  Unknown = 0,
  Exact = 1,
  ExactGivenCondition = 2,
  Inexact = 3,
};

const char* to_string(ExactnessHint h) noexcept;

// PRINCIPLES.md §4: tolerances are per output class. The numbers belong in PROBLEM.md beside the
// outputs they govern; the measured naive-path errors above are what they should be set FROM.
enum class OutputClass : std::uint8_t {
  Valuation = 0,    // Stage A naive error ~1.6e-13
  Sensitivity = 1,  // ~2.1e-10
  Diagnostic = 2,
  Count_ = 3,
};

const char* to_string(OutputClass c) noexcept;

struct ErrorBudget {
  double relative[static_cast<int>(OutputClass::Count_)] = {0.0, 0.0, 0.0};

  // The zero budget. Admits only rewrites whose predicted error is identically zero. Useful to
  // bisect a regression and as a test fixture. It is NOT "the safe setting": it rejects
  // `fma_contract`, `mul_recip_as_div` and the telescope, every one of which is strictly MORE
  // accurate than what it replaces — the failure PRINCIPLES.md §6 item 4 records.
  static ErrorBudget no_rewrites() noexcept { return ErrorBudget{}; }

  // The naive path's own measured error, per class (D72). Offered as a NAMED REFERENCE POINT, not
  // as a default: a rewrite inside this budget does not move the answer by more than the answer
  // was already moving. Deliberately a function rather than a constant, so that re-measuring on a
  // new fingerprint updates one place.
  static ErrorBudget naive_path_stage_a() noexcept { return ErrorBudget{{1.6e-13, 2.1e-10, 2.1e-10}}; }
};

struct OutputClassMap {
  std::vector<OutputClass> by_ordinal;
  OutputClass of(int ordinal) const noexcept {
    const auto i = static_cast<std::size_t>(ordinal);
    return i < by_ordinal.size() ? by_ordinal[i] : OutputClass::Valuation;  // tightest by default
  }
};

// One reverse pass over the ORIGINAL tape, per output class, giving |d(output)/d(node)| for every
// node the graph holds. Built once and reused for every candidate — that reuse is why this is
// affordable, and it is also an approximation worth naming: the sensitivities are those of the
// original tape, not of each candidate. To first order they agree, which is the order the whole
// model is stated at.
class Propagator {
 public:
  virtual ~Propagator() = default;
  // |d(o)/d(n)| * |n|, maximised over the outputs of `klass`: the factor by which a relative
  // perturbation at `n` becomes an absolute perturbation of an output of that class. 0.0 for a
  // node no output of that class depends on — useful in itself, since a rewrite in
  // dead-for-this-class territory is free.
  virtual double weight(node_id n, OutputClass klass) const = 0;
  virtual double output_scale(OutputClass klass) const = 0;
};

// A candidate's accumulated error: the LIST, not a running total. The composition needs the
// per-site adjoint weight, which is not known when the rewrite is made; and a failed oracle check
// must be attributable to a specific rewrite rather than to a number.
struct CandidateError {
  struct Contribution {
    node_id site = invalid_node;
    ErrorTerm term{};
    std::string rule;
  };
  std::vector<Contribution> contributions;

  bool all_exact() const noexcept {
    for (const Contribution& c : contributions) {
      if (!c.term.exact) return false;
    }
    return true;
  }
};

double predicted_relative_error(const CandidateError& err, const Propagator& prop, OutputClass klass);
bool within_budget(const CandidateError& err, const Propagator& prop, const ErrorBudget& budget);

// --------------------------------------------------------------------------------------------
// The slow/fast path agreement test (PRINCIPLES.md §4)
// --------------------------------------------------------------------------------------------
//
// "We need to test if our slow and fast paths agree to within floating point tolerance." Execution
// stopped being a tier, so what used to be an E0 equality assertion becomes a TOLERANCE
// comparison. Three deliberate properties:
//
//   * it carries the NUMBER, not a boolean, because §4 notes the test measures conditioning for
//     free — agreement at 1e-16 says well conditioned, agreement at 1e-9 says one path is losing
//     precision and nothing is broken. Given the 656x valuation/sensitivity spread above, that
//     number is worth tracking per pair over time;
//   * PREFER oracle-against-path to path-against-path wherever the oracle is affordable: it is the
//     stronger statement and it says WHICH path is wrong. `Wide` makes that affordable far more
//     often than it was when this was written;
//   * `exact` defaults to false. Exactness is observed, never assumed.
enum class PathKind : std::uint8_t {
  GenericInterpreter = 0,
  CatalogueKernel = 1,
  Unfused = 2,
  Fused = 3,
  Adjoint = 4,
  ForwardDual = 5,
  Oracle = 6,          // epykos::Wide (D72)
  BeforeReduction = 7, // the tape as the passes left it
  AfterReduction = 8,  // the tape this stage produced
};

const char* to_string(PathKind p) noexcept;

struct PathAgreement {
  PathKind a = PathKind::BeforeReduction;
  PathKind b = PathKind::AfterReduction;
  double max_relative = 0.0;
  int worst_output = -1;
  bool within_tolerance = true;
  bool exact = false;
};

PathAgreement compare_paths(PathKind a, PathKind b, const ErrorBudget& budget, const OutputClassMap& classes);

}  // namespace epykos::algebra
