// EpykosEngine — how a rewrite's error is stated, and how errors COMPOSE (P1/term-rewriting
// DESIGN ONLY; D73; docs/PRINCIPLES.md §4 as amended 2026-09-25, docs/TERM_REWRITING.md §5).
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it.
//
// ---------------------------------------------------------------------------------------------
// Bit-identity is RETIRED as a contract, so this file is the only admission criterion there is
// ---------------------------------------------------------------------------------------------
//
// PRINCIPLES.md §4 (owner, 2026-09-25: "I genuinely don't think we care about bitwise correctness
// if we've got deterministic algebraic equivalence") leaves two tiers and a test. STRUCTURAL is
// exact because it is a graph identity and rounding does not arise. MATHEMATICAL is characterised
// error against the oracle — and it now covers **every execution path too**, so the interpreter,
// the catalogue kernels and the adjoint are implementations of the recorded maths like any
// rewrite and are judged the same way. There is no execution tier and no bit-preserving class.
//
// The consequence for this interface is the whole point: **a rewrite is never admissible BECAUSE
// it preserves bits.** It is admissible when its composed error fits the output class's budget.
// `rewrite::Exactness` survives only as a BUG DETECTOR — see `ExactnessHint` below — and never
// as a licence. Anything that reads an E0 declaration and concludes "therefore admit" is wrong
// under the current contract.
//
// ---------------------------------------------------------------------------------------------
// The problem: a per-op ulp count does not compose
// ---------------------------------------------------------------------------------------------
//
// M1-M4 classified a rewrite as E0 (bit-identical) or E1 ("<= 1 ulp per op", D26/D30). Even as an
// admission criterion that was the wrong instrument, and for COMPOSING several rewrites it is
// useless, for two reasons worth stating separately because they have different fixes:
//
//   (a) ULPS ARE NOT A SCALE. An ulp is a property of a representable neighbourhood, so "1 ulp"
//       at 1e-300 and "1 ulp" at 1e+300 are incomparable quantities. Adding them is meaningless.
//       Relative error is a scale; ulps are relative error divided by a varying quantum.
//
//   (b) EVEN RELATIVE ERROR DOES NOT COMPOSE THROUGH A CHAIN. Multiplicative steps compose
//       benignly ((1+d1)(1+d2) ~ 1 + d1 + d2), but a SUBTRACTION OF NEARBY QUANTITIES amplifies
//       the relative error of its operands by the condition number |a|+|b| over |a-b|, which is
//       unbounded. Stage A contains exactly this shape and it is not incidental: the recorded
//       forward rate is `(DF_s/DF_e - 1) / tau`, where DF_s/DF_e is within about 1e-4 of 1, so
//       the subtraction discards roughly thirteen significant digits and multiplies whatever
//       error reached it by about 1e4. A sum of per-op bounds is off by that factor.
//
// ---------------------------------------------------------------------------------------------
// What DOES compose: first-order propagation by the adjoint the engine already has
// ---------------------------------------------------------------------------------------------
//
// Model each rewrite as an ABSOLUTE perturbation `dt` injected at the term it replaces, and
// propagate it to the outputs by the first-order sensitivity:
//
//     d(output o)  =  SUM over rewritten terms t  of  |d(o)/d(t)| * |dt|
//
// This composes because it is a linear functional, and it is nearly free HERE, specifically,
// for a reason peculiar to this engine: `d(o)/d(t)` is exactly what the mechanical adjoint
// computes (M2, D31), on the program that is already recorded, in one reverse pass per output
// seed. No new machinery, no per-rule error algebra, no interval arithmetic. `docs/PRINCIPLES.md`
// §4 makes the templated oracle available; this makes the SEARCH able to price accuracy without
// running the oracle on every candidate.
//
// Stated as sharply as possible, because it is the part most likely to be over-trusted:
//
//     THE ADJOINT-WEIGHTED NUMBER IS AN ESTIMATE, NOT A BOUND. It is a linearisation. It is the
//     SEARCH HEURISTIC. The GATE is PRINCIPLES.md §4's oracle, run on the ONE extracted
//     candidate. Design, not assertion: the claim that the estimate ranks candidates well enough
//     to pick the right one is unproven and `docs/TERM_REWRITING.md` §8 names the experiment
//     that would test it.
//
// Two places the linearisation is known to be wrong, both handled explicitly rather than hoped
// away:
//
//   - ACROSS A Select. The derivative of a Select with respect to its predicate is zero, so a
//     rewrite under a Select that could flip the branch has a first-order effect of zero and a
//     real effect of the difference between the arms. `ErrorTerm::across_select` marks such a
//     rewrite and the propagator must bound it by the max over arms instead of differentiating.
//   - WHERE THE TERM'S VALUE CAN VANISH. Relative error is undefined at zero, so a term that is
//     not `provably_nonzero` (term.hpp) must state `absolute` and leave `relative` at zero.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"

namespace epykos::rewrite {

// The LOCAL error one rewrite introduces at its own site, against PRINCIPLES.md §4's ground
// truth (the recorded expression at infinite precision) — NOT against the naive `double`
// evaluation, which is merely one realisation of it and may itself be the worse of the two.
//
// That sign convention matters and is the point of §4: `fma_contraction` and
// `mul(a, recip(b)) -> div(a, b)` both REDUCE error, and under the M1-M4 contract both were
// failures. Here they carry a negative `relative_delta` and extraction may prefer them.
struct ErrorTerm {
  // True: this rewrite is an identity in IEEE-754 double for every input, so `relative_delta` is
  // exactly zero and no propagation is needed. Corresponds to Exactness::E0.
  bool exact = true;

  // Bound (or, where a bound is unavailable, a conservative estimate — `bound_is_proved` says
  // which) on the relative change this rewrite makes to its own term's value, at the recording
  // point. Signed: negative means the rewritten form is CLOSER to the infinite-precision value.
  double relative_delta = 0.0;

  // For terms whose value may be zero: the absolute change instead. Used when
  // TermView::provably_nonzero is false for the site.
  double absolute_delta = 0.0;

  // False: `relative_delta` is a modelled estimate (e.g. "n-term reassociation, standard
  // n*eps forward bound"), not a proof. Extraction may still use it; the oracle gate does not
  // care either way, and a candidate built only from proved terms can skip a re-measurement.
  bool bound_is_proved = false;

  // The site sits under a Select and the rewrite could change which arm is taken. First-order
  // propagation is INVALID here; the propagator must fall back to the arm difference.
  bool across_select = false;

  static ErrorTerm zero() noexcept { return ErrorTerm{}; }
};

// PRINCIPLES.md §4: "Tolerances are per output class, not global. Valuation is held tight;
// sensitivities are allowed more." The classes themselves and their numbers belong beside the
// outputs they govern in PROBLEM.md, not here; this is only the handle.
enum class OutputClass : std::uint8_t {
  Valuation = 0,      // PVs: tight
  Sensitivity = 1,    // deltas, vegas, the risk ladder: looser
  Diagnostic = 2,     // convergence diagnostics, residuals: loosest
  Count_ = 3,
};

const char* to_string(OutputClass c) noexcept;

// The per-class budget a candidate must fit inside to be extractable at all. Extraction is
// therefore CONSTRAINED SINGLE-OBJECTIVE (cheapest program whose predicted error fits), not
// genuinely two-objective: the accuracy axis is a constraint the owner sets, the speed axis is
// what the search minimises. docs/TERM_REWRITING.md §5.3 argues why that is the right shape and
// what it gives up.
//
// The numbers live in PROBLEM.md beside the outputs they govern (PRINCIPLES.md §4). There is
// deliberately NO default budget constant here: a budget is an owner decision, and a library
// default would become the de-facto contract the way bit-identity did — PRINCIPLES.md §0's
// "the space the search may explore was never written down" is precisely this failure mode.
// `no_rewrites()` exists for tests and bisection, is named for what it does rather than for a
// property it preserves, and is NOT the contract.
struct ErrorBudget {
  double relative[static_cast<int>(OutputClass::Count_)] = {0.0, 0.0, 0.0};

  // A zero budget everywhere. Admits only rewrites whose predicted error is identically zero,
  // which in practice means the structural and exactly-representable ones. Useful to bisect a
  // regression ("does it still happen with the algebra switched off?") and as a test fixture.
  // It is NOT "the safe setting": a zero budget can REJECT a rewrite that is strictly MORE
  // accurate than what it replaces (`fma_contract`, `mul_recip_as_div`, the telescope), which is
  // exactly the failure PRINCIPLES.md §6 item 4 records.
  static ErrorBudget no_rewrites() noexcept { return ErrorBudget{}; }
};

// Whether a rewrite HAPPENS to be exact in IEEE-754 double. Advisory, and only ever used two
// ways, both of them tests rather than gates (PRINCIPLES.md §4: "a test of convenience, never a
// constraint"):
//
//   * as a FREE ASSERTION. Where a rewrite is exact and asserting it costs nothing, assert it —
//     it is a good bug detector, and it is what caught the GCC operand-order divergence (D61).
//     The moment the rewrite or a kernel under it wants to reorder for speed, the assertion is
//     relaxed THERE and the error measured; it does not propagate a constraint outward.
//   * as a SHORTCUT. A candidate whose every contribution is `Exact` needs no propagator pass,
//     because its composed error is identically zero.
//
// What it must never do is decide admissibility. `Unknown` is the honest default for a rule that
// has not characterised itself and costs only a propagator call.
enum class ExactnessHint : std::uint8_t {
  Unknown = 0,       // not characterised; treat as inexact and measure
  Exact = 1,         // identical bits for every input, unconditionally
  ExactGivenCondition = 2,  // identical bits once a row-universal side condition is discharged
  Inexact = 3,       // a real error, stated in the ErrorTerm
};

const char* to_string(ExactnessHint h) noexcept;

// Which output class each output ordinal belongs to. Supplied by the caller (the Stage A harness
// knows its own output layout, D44); the optimiser never guesses.
struct OutputClassMap {
  std::vector<OutputClass> by_ordinal;

  OutputClass of(int ordinal) const noexcept {
    const auto i = static_cast<std::size_t>(ordinal);
    return i < by_ordinal.size() ? by_ordinal[i] : OutputClass::Valuation;  // tightest by default
  }
};

// One reverse pass over the ORIGINAL program, per output class, giving |d(output)/d(term)| for
// every term the graph holds. Built once before extraction and reused for every candidate — that
// reuse is the reason this is affordable, and it is also an APPROXIMATION worth naming: the
// sensitivities are those of the ORIGINAL program, not of each candidate. To first order they
// agree, which is the same order the whole model is stated at.
class ErrorPropagator {
 public:
  virtual ~ErrorPropagator() = default;

  // Largest |d(o)/d(t)| * |t| over the outputs of `klass` and over the rows of t's anchor
  // domain, i.e. the factor by which a relative perturbation at `t` shows up as an absolute
  // perturbation of an output of that class. Returns 0.0 for a term no output of that class
  // depends on — which is itself useful: a rewrite in dead-for-this-class territory is free.
  virtual double weight(ir::domain_id anchor, std::int32_t step, OutputClass klass) const = 0;

  // The scale the resulting absolute error is judged relative to for that class (the class's own
  // |output| scale, or D26's leg-scale convention where a raw |pv| is not the right denominator).
  virtual double output_scale(OutputClass klass) const = 0;
};

// A candidate's accumulated error: one entry per rewrite in its derivation, composed on demand.
// Kept as the LIST, not as a running total, for two reasons: the composition needs the per-site
// adjoint weight (which is not known when the rewrite is made), and a failed oracle check must be
// attributable to a specific rewrite rather than to a number.
struct CandidateError {
  struct Contribution {
    ir::domain_id anchor = -1;
    std::int32_t step = -1;
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

// The composed, first-order predicted relative error of `err` on `klass`. Declared here, not
// defined: this header is a proposal.
double predicted_relative_error(const CandidateError& err, const ErrorPropagator& prop, OutputClass klass);

// True when every class's predicted error fits its budget. An all-exact candidate fits every
// budget without consulting the propagator at all.
bool within_budget(const CandidateError& err, const ErrorPropagator& prop, const ErrorBudget& budget);

// --------------------------------------------------------------------------------------------
// The slow/fast path agreement test (PRINCIPLES.md §4, owner 2026-09-25)
// --------------------------------------------------------------------------------------------
//
// "We need to test if our slow and fast paths agree to within floating point tolerance."
// Execution stopped being a tier, so what used to be an E0 equality assertion between the generic
// interpreter and a catalogued kernel becomes a TOLERANCE comparison, and it belongs here rather
// than in the verifier because it is the same error vocabulary.
//
// The owner's own objection, answered in §4: if the algebraic engine is correct they agree by
// construction. True in R, false in floating point — two algebraically equivalent expressions are
// different sequences of roundings — and, more to the point, the test is not testing the algebra.
// It tests that the CODE implements the algebra, which is a claim about the construction rather
// than a consequence of it. Two of this week's defects were exactly that gap and neither was an
// algebra failure (D61's catalogue fingerprint that was canonical by design and was not; D65's
// harness that sized a buffer from the wrong vector).
//
// It also measures CONDITIONING for free, which is why the result carries the number and not just
// a boolean: agreement at 1e-16 says the expression is well conditioned; agreement at 1e-9 says
// one path is losing precision and nothing is broken. That number is worth recording per pair
// over time — it is the cheapest conditioning monitor the engine will ever have.
enum class PathKind : std::uint8_t {
  GenericInterpreter = 0,  // exec::Interpreter, no catalogue
  CatalogueKernel = 1,     // a bound catalogue signature
  Unfused = 2,             // planner fusion off
  Fused = 3,               // planner fusion on
  Adjoint = 4,
  ForwardDual = 5,
  Oracle = 6,              // the wide-precision instantiation (PRINCIPLES.md §4)
};

const char* to_string(PathKind p) noexcept;

struct PathAgreement {
  PathKind a = PathKind::GenericInterpreter;
  PathKind b = PathKind::CatalogueKernel;
  double max_relative = 0.0;   // worst disagreement seen, relative to the output class's scale
  int worst_output = -1;
  bool within_tolerance = true;
  // True when the two paths happened to agree EXACTLY. Recorded, never required: §4 allows
  // asserting it where it costs nothing, as a bug detector. A pair that has always been exact
  // and stops being exact is worth a look and is not by itself a failure.
  bool exact = false;
};

// PREFER measuring each path against the oracle to comparing two paths with each other, wherever
// the oracle is affordable: it is the stronger statement and it says WHICH path is wrong rather
// than merely that they differ (PRINCIPLES.md §4). Path-against-path is the cheap form and
// belongs where the oracle is too expensive — which, for a 1,000-lane Stage A scenario grid, is
// most places.
PathAgreement compare_paths(PathKind a, PathKind b, const ErrorBudget& budget, const OutputClassMap& classes);

}  // namespace epykos::rewrite
