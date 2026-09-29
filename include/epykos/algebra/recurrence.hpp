// EpykosEngine — the recurrence rule KIND of PRINCIPLES.md §2a, over tape chains (P1; DESIGN
// ONLY; D73). `docs/TERM_REWRITING.md` §4.
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it.
//
// ---------------------------------------------------------------------------------------------
// At the tape, this is an ordinary rule again
// ---------------------------------------------------------------------------------------------
//
// An earlier draft of this design targeted `ir::Program` and concluded the recurrence kind could
// not be a term rule there, because a scan domain's rows are the STEPS of its chains while a
// closed form has one value per CHAIN — so collapsing a recurrence deleted rows, changed a row
// count and re-pointed gathers, which is a domain-level edit. That conclusion was correct for that
// layer and is now moot: at the tape a chain is a chain of NODES, and collapsing it replaces a
// sub-DAG with a smaller sub-DAG. Ordinary. `ir::infer` then simply does not find a scan there,
// because there is no longer a chain to find.
//
// It needs one thing the tape does not carry: knowing a chain is there. `ir/analysis.hpp` is that,
// and the structural argument for how it is obtained is in that header — it is the main open
// design question in this package.
//
// ---------------------------------------------------------------------------------------------
// How a match is PROVED rather than pattern-matched
// ---------------------------------------------------------------------------------------------
//
// `maths/swap/compounding.hpp` records, and says so in its own header:
//
//     acc_{k+1} = acc_k * (1 + f_k * w_k),     f_k = (DF(t_rate_k) / DF(t_next_k) - 1) / tau_k
//
// "with r_i = (DF(s_i)/DF(e_i) - 1)/tau_i it collapses to DF(s)/DF(e) on paper". On paper. Whether
// it collapses in THIS RECORDING is three facts, all decidable exactly from the tape:
//
//   (P1) SHAPE. The step is `mul(carry, X)` and X reduces to `div(g_num, g_den)` modulo the affine
//        rearrangement the recorder and `affine_collapse` left behind. Ordinary matching, and the
//        weakest of the three.
//
//   (P2) THE INDEX CONDITION — the telescope itself. The factor must be a ratio of CONSECUTIVE
//        terms of one indexed family: step k's denominator must be step k+1's numerator. At the
//        tape this is
//
//            denominator_node(step k) == numerator_node(step k + 1)
//
//        — **identical node ids**, not merely equal values. `cse` has already merged every node
//        with the same op, operands and constant bit pattern, so if the two discount factors are
//        the same computation they ARE the same node, and the check is an integer comparison. This
//        is cleaner than the IR-layer form (which compared two gather index arrays) and it is
//        cleaner than a numerical test could ever be: it establishes that the ratio cancels
//        exactly in R, by identity rather than by tolerance.
//
//   (P3) THE COEFFICIENT CONDITION. `1 + f_k*w_k` equals `DF_k/DF_{k+1}` only when the compounding
//        weight equals the forward's accrual, `w_k == tau_k`. These are two Const leaves per step
//        (`ObsDay::weight` and `ObsDay::tau_rate`, `maths/instrument/tables.hpp`) and the check is
//        a comparison of their bit patterns — which, again, `cse` has already made an identity
//        check: equal constants are ONE node.
//
// (P4) LIVENESS is a use count, from `ir::Analysis::uses`: collapsing the chain to its endpoints
// is legal only when nothing outside reads an intermediate step.
//
// ---------------------------------------------------------------------------------------------
// (P3) is not a formality — the Stage A book is a mix
// ---------------------------------------------------------------------------------------------
//
// `tables.hpp` states the finance: "plain: n_i = r' - r and the product telescopes; lookback /
// observation shift / lockout: the span and the weight differ, and it does not." Reading
// `src/conventions/rfr.cpp` and `src/maths/instrument/builder.cpp` against
// `blueprints/problems/stage_a.json` (a code-reading claim, not a measurement — see
// docs/TERM_REWRITING.md §8.3 for the check):
//
//   USD-SOFR-OIS plain      25%   (P2) ok  (P3) ok   telescopes
//   EUR-ESTR-OIS plain      15%   ok       ok        telescopes
//   USD-SOFR-OIS-SHIFT2     10%   ok       ok        telescopes — `ObservationShift` moves
//                                                    `obs_start` and `obs_end` TOGETHER, so weight
//                                                    and rate span still agree. The `tables.hpp`
//                                                    comment is over-broad about this one.
//   ...-SHIFT2-LOCKOUT2     10%   ok on a prefix     PARTIAL: lockout freezes the last two rate
//                                                    dates, so (P2) fails on the tail only.
//   USD-SOFR-AVG-SWAP       10%   n/a                an arithmetic average, not a product.
//   EURIBOR / 3s6s          30%   n/a                term rate, no chain.
//
// A two-day LOOKBACK blueprint exists in the instrument set and is NOT drawn by the Stage A mix.
// It passes (P2) and FAILS (P3) — the rate date moves back independently of the accrual day — so
// it is the classifier's best negative test precisely because the cheap half of the proof succeeds
// on it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "epykos/algebra/error.hpp"
#include "epykos/algebra/rule.hpp"
#include "epykos/ir/analysis.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::algebra {

// PRINCIPLES.md §2a's library in full: "Product of consecutive ratios (telescoping). Geometric.
// Arithmetic. Linear with constant coefficients. That list is the work, and it is finite."
//
// What the list is WORTH over a frozen op set differs by an order of magnitude between the first
// entry and the rest (docs/TERM_REWRITING.md §4.4). Only `ConsecutiveRatioProduct` gets an
// ASYMPTOTIC win without a new primitive: the other two non-trivial classes need r^n, which the op
// set does not have and which must be spelled `exp(n log r)` — legal, inexact, requiring
// `provably_positive(r)`, and trading n multiplies for two transcendentals.
enum class RecurrenceClass : std::uint8_t {
  None = 0,
  ConsecutiveRatioProduct = 1,   // x_{k+1} = x_k * (a_k / a_{k+1})  =>  x_n = x_0 * a_0 / a_n
  Geometric = 2,                 // x_{k+1} = r * x_k                =>  x_n = x_0 * r^n
  Arithmetic = 3,                // x_{k+1} = x_k + d                =>  x_n = x_0 + n*d
  LinearConstantCoefficient = 4, // x_{k+1} = a*x_k + b
  Count_ = 5,
};

const char* to_string(RecurrenceClass c) noexcept;

// True when closing this class needs `exp(n log r)` because the op set has no power op
// (PRINCIPLES.md §2: "the op set does not grow for this").
constexpr bool recurrence_needs_exp_log(RecurrenceClass c) noexcept {
  return c == RecurrenceClass::Geometric || c == RecurrenceClass::LinearConstantCoefficient;
}

// The evidence. EVERY field is an exact check on the tape: node-id identity, constant bit
// patterns, use counts. No sampling, no tolerance, no numerical experiment. That is what "PROVED
// rather than pattern-matched" means operationally, and it is also what makes the result
// reproducible and mutation-testable — every `false` is a nameable obligation a mutant can skip.
struct Proof {
  RecurrenceClass klass = RecurrenceClass::None;
  std::int32_t chain = -1;      // index into ir::Analysis::chains

  bool step_shape_matched = false;          // (P1)

  // (P2): denominator_node(step k) == numerator_node(step k+1), by node id, for every covered
  // step. `covered_steps` is how many leading steps the condition holds for — equal to the chain
  // length normally, SHORTER for a lockout tail. A chain covered for fewer than three steps is
  // not worth closing and the classifier reports 0.
  bool index_chain_consecutive = false;
  std::int32_t covered_steps = 0;

  // (P3): the coefficient constants the closed form requires to be equal ARE equal. After `cse`
  // this is node-id identity; the fields record which nodes were compared so a failure is legible.
  bool coefficient_constants_equal = false;
  node_id coefficient_a = invalid_node;
  node_id coefficient_b = invalid_node;

  // (P4): nothing outside the chain reads an intermediate step (ir::Analysis::uses).
  bool only_endpoint_read = false;

  // Written whether the classification succeeds or fails. D53 requires a package to state whether
  // its deliverable actually fires, and "it did not, and here is the obligation that failed" is
  // the useful form of that.
  std::string account;

  bool holds() const noexcept { return klass != RecurrenceClass::None && step_shape_matched; }
  bool collapses_whole_chain() const noexcept { return holds() && only_endpoint_read; }
};

// Which closed form to emit.
enum class Closure : std::uint8_t {
  // Replace the chain's steps with the endpoint expression. Requires (P4). The 250:1 form.
  Endpoints = 0,
  // Keep one node per step, each the closed form at that step (x_k = a_0 / a_k). Same node count,
  // a divide instead of a multiply, but NO SEQUENTIAL DEPENDENCE — and that is worth more than it
  // sounds: `ir::infer` will no longer see a chain, so the rows stop being a scan domain, and
  // "a scan is never fused or inlined" (D41). Measured relevance: the Stage A compounding scan
  // d10 is 255,687 rows and **69% of the B=64 run, 74% at B=1** (M3-gate-1 planner coverage).
  PerStep = 1,
};

const char* to_string(Closure c) noexcept;

// The recurrence rule kind. It IS an algebra::Rule — see this header's opening — with two extra
// virtuals that make the proof inspectable rather than buried inside `build`. A driver that knows
// nothing about recurrences runs it as a plain Rule; a test, a report or the owner can ask it why.
class RecurrenceRule : public Rule {
 public:
  virtual std::vector<RecurrenceClass> classes() const = 0;

  // Analyse one detected chain and return the proof, held or not. Pure, const, deterministic;
  // `account` is filled in either way. This is what `search` is implemented in terms of and what a
  // gate test calls directly.
  virtual Proof classify(const Tape& tape, const ir::Analysis& analysis, std::int32_t chain) const = 0;

  // Whether this closure is available under this proof and what it costs in error. The rule does
  // not decide whether to apply it — extraction does, under a budget — so both are offered
  // whenever both are legal.
  virtual std::optional<ErrorTerm> closure_error(const Proof& proof, Closure closure) const = 0;
};

}  // namespace epykos::algebra
