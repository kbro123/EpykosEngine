// EpykosEngine — the recurrence rule KIND of PRINCIPLES.md §2a (P1/term-rewriting DESIGN ONLY;
// D73; docs/TERM_REWRITING.md §4).
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it.
//
// ---------------------------------------------------------------------------------------------
// Why this is not a TermRule, which is the finding, not the caveat
// ---------------------------------------------------------------------------------------------
//
// PRINCIPLES.md §2a asks for "a rule KIND it does not have: one that analyses a detected scan's
// step and recognises it as a member of a known recurrence class, rather than pattern-matching a
// term". Working the shape through, the rule kind it needs is NOT term-level, for a reason that
// is structural rather than incidental:
//
//   A scan domain's rows are the STEPS of its chains (D41: chain-major, `chain_offsets`). The
//   closed form of a chain has ONE value per CHAIN, not one per step. So collapsing a recurrence
//   deletes rows, changes a domain's row count, and re-points every gather that read it. That is
//   a DOMAIN-LEVEL edit — the same kind of edit `r5.group_formation` and `r7.block_linmap` make —
//   and it is precisely what a term rewrite is defined not to do (term.hpp point 1: a term
//   rewrite applies to every row and changes no row's existence).
//
// So the recurrence kind slots into the EXISTING `rewrite::Rule` interface (whole-Program
// Proposal), and that is fine, because the combinatorial objection of PRINCIPLES.md §6 item 1
// does not apply to it: it has exactly ONE site per scan domain, and the full Stage A tape has
// FIVE scan domains (D35/D44). Five Programs, not 10,111.
//
// The consequence for sequencing, which the owner should see plainly: PRINCIPLES.md §8 orders
// term-level rewriting (item 2) before the recurrence kind and telescoping (items 3 and 4). The
// recurrence kind does not depend on item 2 at all. Telescoping — the 250:1 worked example — can
// be attempted FIRST, on the existing interface, and would then be the evidence that decides
// whether the term layer is worth its six thousand lines. `docs/TERM_REWRITING.md` §8.1.
//
// ---------------------------------------------------------------------------------------------
// How a match is PROVED rather than pattern-matched
// ---------------------------------------------------------------------------------------------
//
// The step shape is necessary and nowhere near sufficient. Take the class this engine was built
// for. `maths/swap/compounding.hpp` records
//
//     acc_{k+1} = acc_k * (1 + f_k * w_k),     f_k = (DF(t_rate_k) / DF(t_next_k) - 1) / tau_k
//
// and says in its own header that "with r_i = (DF(s_i)/DF(e_i) - 1)/tau_i it collapses to
// DF(s)/DF(e) on paper". On paper. Whether it collapses in THIS RECORDING is three separate
// facts, and all three are decidable exactly, from integer and double tables already in the
// `ir::Program`, with no sampling and no numerical experiment:
//
//   (P1) SHAPE. The scan group's step is `mul(carry, X)` and X reduces to `div(g_num, g_den)`
//        modulo the affine rearrangement the recorder left behind. Ordinary matching.
//
//   (P2) THE INDEX CONDITION — the telescope itself. The factor must be a ratio of CONSECUTIVE
//        terms of one indexed family, i.e. row k's denominator must be row k+1's numerator:
//
//             gathers[g_den].index[r] == gathers[g_num].index[r + 1]
//
//        for every row r inside a chain. This is an equality of `std::int32_t` value ids in a
//        table the Program already holds. It is a PROOF, not a pattern: it establishes that the
//        two DFs are literally the SAME recorded value, so their ratio cancels exactly in R.
//
//   (P3) THE COEFFICIENT CONDITION. `1 + f_k * w_k` equals `DF_k / DF_{k+1}` only when the
//        compounding weight equals the forward's accrual, i.e. `w_k == tau_k` for every row.
//        These are two different Columns (`ObsDay::weight` and `ObsDay::tau_rate`,
//        maths/instrument/tables.hpp) and the check is a bitwise comparison of two double
//        vectors.
//
// (P3) is not a formality. `tables.hpp` states the finance: "plain: n_i = r' - r and the product
// telescopes; lookback / observation shift / lockout: the span and the weight differ, and it does
// not." The Stage A book (blueprints/problems/stage_a.json) is a MIX: plain SOFR OIS and ESTR OIS
// telescope; the two-day-lookback blueprint fails (P3) while PASSING (P2), which is exactly the
// case a pattern matcher would get wrong; the lockout blueprint passes both on a PREFIX of each
// chain and fails on the frozen tail, because lockout repeats one rate date and breaks (P2)
// there. Hence `RecurrenceProof::prefix_steps`: the rule must be able to close a prefix and leave
// a tail, or it will decline on a tenth of the book for no good reason.
//
// A fourth obligation is about the SURROUNDINGS rather than the recurrence:
//
//   (P4) LIVENESS. Replacing 90 rows by 1 is only legal if nothing outside the domain reads the
//        intermediate rows. Decidable by scanning every other domain's gather index arrays and
//        the output list. When intermediates ARE read, the closed form still exists row by row
//        (x_k = DF(t_0)/DF(t_k), one div per row instead of one mul) — the same row count, worse
//        arithmetic, but PARALLEL instead of a sequential wave-by-wave scan (D41's execution
//        model). Whether that pays is the cost model's question, not the rule's, so the interface
//        carries both closures rather than choosing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/rewrite/error_model.hpp"
#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

// PRINCIPLES.md §2a's library, in full: "Product of consecutive ratios (telescoping). Geometric.
// Arithmetic. Linear with constant coefficients. That list is the work, and it is finite."
//
// What the list is WORTH over a frozen op set differs by an order of magnitude between the first
// entry and the rest, and `docs/TERM_REWRITING.md` §4.4 gives the arithmetic. In short: only
// ConsecutiveRatioProduct gets an ASYMPTOTIC win (n steps -> 1 divide) without a new primitive.
// Geometric and LinearConstantCoefficient both need r^n, which the op set does not have and which
// must be spelled `exp(n * log(r))` — legal, but it needs `provably_positive(r)`, it is E1 with
// real error, and it turns ~n multiplies into one exp and one log, so the win is a small constant
// factor, not 250:1. Arithmetic collapses honestly (n adds -> a Fma over a per-row k Column) and
// is the second-best entry.
enum class RecurrenceClass : std::uint8_t {
  None = 0,
  // x_{k+1} = x_k * (a_{k} / a_{k+1})  =>  x_n = x_0 * a_0 / a_n. The telescope.
  ConsecutiveRatioProduct = 1,
  // x_{k+1} = r * x_k, r uniform  =>  x_n = x_0 * r^n. Needs exp/log; see above.
  Geometric = 2,
  // x_{k+1} = x_k + d, d uniform  =>  x_n = x_0 + n*d.
  Arithmetic = 3,
  // x_{k+1} = a*x_k + b, a and b uniform  =>  x_n = a^n x_0 + b (a^n - 1)/(a - 1). Needs exp/log
  // AND provably a != 1; degenerates to Arithmetic at a == 1.
  LinearConstantCoefficient = 4,
  Count_ = 5,
};

const char* to_string(RecurrenceClass c) noexcept;

// True when closing this class needs `exp(n log r)` because the op set has no power op
// (PRINCIPLES.md §2: "the op set does not grow for this"). Such a closure is E1 and needs
// `provably_positive` on the base.
constexpr bool recurrence_needs_exp_log(RecurrenceClass c) noexcept {
  return c == RecurrenceClass::Geometric || c == RecurrenceClass::LinearConstantCoefficient;
}

// The evidence for a classification. EVERY field is the outcome of an exact check against data
// already in the `ir::Program`: integer index arrays, double Column contents, the scan's own
// `chain_offsets`. No sampling, no tolerance, no numerical experiment. That is what "PROVED
// rather than pattern-matched" means operationally, and it is also what makes the result
// reproducible and mutation-testable: every `false` below is a specific, nameable obligation a
// mutant can be made to skip.
struct RecurrenceProof {
  RecurrenceClass klass = RecurrenceClass::None;
  ir::domain_id domain = -1;       // the scan domain (Domain::scan >= 0)
  std::int32_t scan = -1;          // index into Program::scans

  // (P1) the step matched the class's shape.
  bool step_shape_matched = false;

  // (P2) for ConsecutiveRatioProduct: gathers[den].index[r] == gathers[num].index[r+1] for every
  // row r covered. `num_gather` / `den_gather` name the two tables the check ran on.
  bool index_chain_consecutive = false;
  std::int32_t num_gather = -1;
  std::int32_t den_gather = -1;

  // (P3) the coefficient columns the closed form requires to be equal ARE equal, bitwise, on
  // every covered row. For the compounded coupon these are ObsDay::weight and ObsDay::tau_rate;
  // the two-day-lookback blueprint is the case that fails here while passing (P2).
  bool coefficient_columns_equal = false;
  std::int32_t coefficient_column_a = -1;
  std::int32_t coefficient_column_b = -1;

  // (P4) nothing outside the domain reads an intermediate row, so the whole chain may collapse to
  // one row. False: only `RecurrenceClosure::PerRow` is available.
  bool only_chain_final_rows_read = false;

  // Which chains, and how much of each. `prefix_steps[c]` is how many leading steps of chain c
  // the proof covers; it equals the chain's length when the whole chain closes, and is SHORTER
  // for a lockout tail (the frozen rate dates repeat, so (P2) fails on the last lockout_days
  // steps). A chain with `prefix_steps[c] < 3` is not worth closing and the classifier reports 0.
  std::vector<std::int32_t> prefix_steps;

  // Human-readable account of what was checked and what, if anything, refused. Written whether
  // the classification succeeds or fails, because D53 requires a package to state whether its
  // deliverable actually fires, and "it did not fire, here is the obligation that failed" is the
  // useful form of that.
  std::string account;

  bool holds() const noexcept { return klass != RecurrenceClass::None && step_shape_matched; }
  // Whether the whole chain closes to a single row, or only row by row.
  bool collapses_rows() const noexcept { return holds() && only_chain_final_rows_read; }
};

// Which closed form to emit.
enum class RecurrenceClosure : std::uint8_t {
  // One row per chain replaces the chain's steps. Requires (P4). The 250:1 form.
  ChainFinal = 0,
  // One row per step, each the closed form at that step (x_k = a_0 / a_k). Same row count, worse
  // arithmetic per row, but no sequential dependency — a parallel domain instead of D41's
  // wave-by-wave scan. Always available when the proof holds.
  PerRow = 1,
};

// The recurrence rule kind. It IS a `rewrite::Rule` — see this header's opening — with two extra
// virtuals that make the proof inspectable rather than buried inside `propose`. A driver that
// knows nothing about recurrences runs it as a plain Rule; a test, a report or the owner can ask
// it why.
class RecurrenceRule : public Rule {
 public:
  // The recurrence classes this rule can recognise.
  virtual std::vector<RecurrenceClass> classes() const = 0;

  // Analyse one scan domain and return the proof, held or not. Pure, const, deterministic; the
  // `account` field is filled in either way. This is the function `match` is implemented in terms
  // of, and the function a gate test calls directly.
  virtual RecurrenceProof classify(const ir::Program& program, ir::domain_id scan_domain) const = 0;

  // Whether this closure is available under this proof, and what it costs in error. The rule
  // does not decide whether to apply it — that is extraction's job under a budget
  // (error_model.hpp) — so both closures are offered whenever both are legal.
  virtual std::optional<ErrorTerm> closure_error(const RecurrenceProof& proof, RecurrenceClosure closure) const = 0;
};

// Every scan domain of `program`, i.e. every site any RecurrenceRule could have. On the full
// Stage A tape this is FIVE. Declared here so the bound is visible at the interface: whatever
// else the recurrence kind risks, a combinatorial site explosion is not among them.
std::vector<ir::domain_id> recurrence_sites(const ir::Program& program);

}  // namespace epykos::rewrite
