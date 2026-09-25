// EpykosEngine — how a rewrite's error is stated, and how errors COMPOSE (P1/term-rewriting
// DESIGN ONLY; D73; docs/PRINCIPLES.md §4, docs/TERM_REWRITING.md §5).
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it.
//
// ---------------------------------------------------------------------------------------------
// The problem: a per-op ulp count does not compose
// ---------------------------------------------------------------------------------------------
//
// M1-M4 classified a rewrite as E0 (bit-identical) or E1 ("<= 1 ulp per op", D26/D30). That is
// adequate for ADMITTING one rewrite and useless for COMPOSING several, for two reasons that are
// worth stating separately because they have different fixes:
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
struct ErrorBudget {
  double relative[static_cast<int>(OutputClass::Count_)] = {0.0, 0.0, 0.0};

  // Every rewrite must be E0. Reproduces the M1-M4 contract exactly, so the existing gates keep
  // meaning and a caller can ask for the old behaviour in one call.
  static ErrorBudget exact_only() noexcept { return ErrorBudget{}; }
};

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

}  // namespace epykos::rewrite
