// EpykosEngine — the term-level rule interface (P1/term-rewriting DESIGN ONLY; D73).
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it. `docs/TERM_REWRITING.md` §1 and §2.
//
// This is `rewrite::Rule` (rule.hpp, D47) with the one change PRINCIPLES.md §6 item 1 demands: a
// rule returns a REPLACEMENT TERM, not a whole `ir::Program`. Everything else about D47's contract
// is kept on purpose, because it is what made D62's scaling work possible:
//
//   * PURITY. `search` and `build` are const, take a const `TermView`, and are total functions of
//     (view, class, binding). D62's application memo is sound ONLY because `Rule::match` /
//     `Rule::propose` are pure; handing rules a mutable graph (the shape egg's `Applier` uses)
//     would delete that argument. The cost is that a rule cannot intern nodes itself, which is
//     why TermExpr (term.hpp) is a data description the DRIVER interns. That is the right trade
//     here and it is the single most consequential difference from an off-the-shelf e-graph.
//   * SEARCH / BUILD SPLIT. `search` enumerates, `build` constructs exactly one. Same two-phase
//     shape as match/propose, so the driver's accounting, logging and bounds carry over
//     unchanged.
//   * DECLARED EXACTNESS. Still CLAUDE.md's E0/E1, still per rule, still enforced by extraction.
//     `ErrorTerm` (error_model.hpp) is ADDITIONAL, not a replacement: E0/E1 says whether the rule
//     may be used at all, `ErrorTerm` says what using it costs.
//
// Three fields are new, and each exists to bound the search rather than to describe the rule
// (`docs/TERM_REWRITING.md` §6):
//
//   `pattern_depth()`   — how deep the rule looks. The driver indexes the graph by op to depth
//                         max(pattern_depth), so search is O(nodes x rules) rather than
//                         O(nodes^depth). A rule wanting depth 6 must be written as two rules.
//   `crosses_domains()` — true when the rule's pattern spans a Gather. Those rules are the
//                         explosion risk (§6.4) and the driver runs them in a separately bounded
//                         phase, not in the main loop.
//   `opens_sites()`     — an honest self-report of how many sites the rule expects per domain.
//                         D65 measured what happens when this is wrong by an order of magnitude
//                         (10,111 sites, 700 s, bound blown). It is advisory: the driver logs the
//                         discrepancy rather than trusting it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/rewrite/error_model.hpp"
#include "epykos/rewrite/rule.hpp"
#include "epykos/rewrite/term.hpp"

namespace epykos::rewrite {

// One match: the class the rule fires ON, plus the classes its pattern variables bound to.
// `bound` is indexed by the rule's own pattern-variable numbering and is what
// `ExprRef::bound(i)` refers to.
struct TermMatch {
  TermClassId site = invalid_term_class;
  std::vector<TermClassId> bound;

  // The e-NODE of `site` that matched. A class holds several forms; the memo is keyed on the
  // node, not the class, because nodes are immutable under hash-consing while classes merge.
  // `docs/TERM_REWRITING.md` §2.5.
  std::int64_t matched_node = -1;

  bool operator==(const TermMatch&) const = default;
};

// How a rule reports matches. A sink rather than a returned vector for one measured reason: D65's
// 10,111-site case must be STOPPABLE mid-enumeration, and a rule that returns a vector has
// already paid for all of it. `accept` returning false means "budget spent, stop searching"; a
// well-behaved rule returns from `search` immediately.
class MatchSink {
 public:
  virtual ~MatchSink() = default;
  virtual bool accept(const TermMatch& m) = 0;
};

class TermRule {
 public:
  virtual ~TermRule() = default;

  // "<family>.<rule>", e.g. "alg.mul_assoc", "alg.exp_product", "gather.push_unary".
  virtual const std::string& name() const noexcept = 0;

  // E0: bit-identical for every row. E1: characterised error, stated per application in the
  // ErrorTerm `build` fills in.
  virtual Exactness exactness_class() const noexcept = 0;

  // Maximum depth of the rule's pattern, in e-nodes, counting the site's own node as 1.
  virtual int pattern_depth() const noexcept = 0;

  // True when the pattern spans a Gather (i.e. reaches into another domain's group).
  virtual bool crosses_domains() const noexcept = 0;

  // Advisory: sites the rule expects per domain it applies to. Used to order rules cheapest-first
  // and to flag a rule whose actual count is wildly larger. 0 means "no estimate".
  virtual std::size_t opens_sites() const noexcept { return 0; }

  // Enumerate matches. Read-only, total, deterministic: the same `view` yields the same matches
  // in the same order, every time. Stops early if the sink declines.
  virtual void search(const TermView& view, MatchSink& sink) const = 0;

  // Build the replacement for exactly one match — one `search` produced; any other is a caller
  // error, not a condition `build` detects. Returns false when the rule declines after all (a
  // side condition the search could not cheaply check turned out to fail): that is legal and is
  // NOT the matcher bug an empty `Proposal` was, because side conditions here are exact scans a
  // searcher may reasonably defer.
  virtual bool build(const TermView& view, const TermMatch& match, TermRewrite& out) const = 0;
};

// --------------------------------------------------------------------------------------------
// The identity set, declared in one place
// --------------------------------------------------------------------------------------------

// Every axiom the engine holds, as a closed enumeration, so that a reviewer can read the list
// rather than grep for rules, and so a mutation test can assert that each one is covered.
// `docs/TERM_REWRITING.md` §3 states each axiom's ops, its exactness, and — where an axiom is NOT
// safe — exactly why. `Op::Sum` and `Op::Affine` are conspicuously almost absent, and that is the
// deliberate part: they are FIXED-ARITY LEFT FOLDS whose reassociation changes rounding, D61 did
// not canonicalise their member order, and AC matching over a segment of hundreds of members is
// how an e-graph dies. §3.4.
enum class Axiom : std::uint16_t {
  // --- exact in IEEE-754 double, unconditionally (E0) ---
  NegNeg,            // neg(neg(x)) = x
  SubAsAddNeg,       // sub(a,b) = add(a, neg(b))
  NegSub,            // neg(sub(a,b)) = sub(b,a)
  MulOne,            // mul(x,1) = x
  DivOne,            // div(x,1) = x
  SelectSameArms,    // select(p,x,x) = x
  SelectPushUnary,   // f(select(p,a,b)) = select(p, f(a), f(b)), f unary; both arms recorded (D5)
  CommuteAddMul,     // add/mul/cmpeq operand order (op_is_commutative already asserts bitwise)

  // --- exact given a side condition TermView can discharge (E0 conditional) ---
  AddZero,           // add(x,0) = x   REQUIRES provably_not_negative_zero(x)
  SubZero,           // sub(x,0) = x   REQUIRES provably_not_negative_zero(x)
  MulZero,           // mul(x,0) = 0   REQUIRES provably_finite(x)   (inf*0 = NaN)
  SubSelf,           // sub(x,x) = 0   REQUIRES provably_finite(x)
  DivSelf,           // div(x,x) = 1   REQUIRES provably_nonzero and provably_finite
  AffineDropZeroCoef,// drop a member with c_i = 0  REQUIRES provably_finite of that member

  // --- identities in R, not in float: E1, each carrying an ErrorTerm ---
  AddAssoc,          // (a+b)+c = a+(b+c)
  MulAssoc,          // (a*b)*c = a*(b*c)
  MulDistribAdd,     // a*(b+c) = a*b + a*c, and the factoring direction, which is the valuable one
  DivAsMulRecip,     // div(a,b) = mul(a, recip(b))            R4b's trade, error UP
  MulRecipAsDiv,     // mul(a, recip(b)) = div(a,b)            the reverse, error DOWN
  FmaContract,       // add(mul(a,b),c) = fma(a,b,c)           error DOWN; already a rule
  FmaExpand,         // the reverse                            error UP
  RecipRecip,        // recip(recip(x)) = x
  ExpProduct,        // mul(exp(a),exp(b)) = exp(add(a,b))     n exps -> 1: the biggest single win
  ExpSumSegment,     // product of a Sum's worth of exps -> exp of their Sum
  LogProduct,        // log(mul(a,b)) = add(log a, log b)      REQUIRES provably_positive on both
  LogRecip,          // log(recip(a)) = neg(log a)             REQUIRES provably_positive
  ExpLog,            // exp(log(a)) = a                        REQUIRES provably_positive
  LogExp,            // log(exp(a)) = a                        REQUIRES no overflow in exp(a)
  SqrtProduct,       // mul(sqrt a, sqrt b) = sqrt(mul(a,b))   REQUIRES both provably non-negative
  SumFactorCommon,   // Sum(c*x_0 .. c*x_n) = c * Sum(x_0..x_n)  removes n multiplies

  // --- crossing a gather: the bridge, not congruence (term.hpp's header) ---
  GatherPushUnary,   // gath(g, f(t)) = f(gath(g, t)), f elementwise
  GatherPushBinary,  // gath(g, f(s,t)) = f(gath(g,s), gath(g,t)), same g both sides

  Count_,
};

const char* to_string(Axiom a) noexcept;

// Whether the axiom is exact in IEEE-754 double for every input (E0 unconditionally).
constexpr bool axiom_is_unconditionally_exact(Axiom a) noexcept {
  return a == Axiom::NegNeg || a == Axiom::SubAsAddNeg || a == Axiom::NegSub || a == Axiom::MulOne ||
         a == Axiom::DivOne || a == Axiom::SelectSameArms || a == Axiom::SelectPushUnary ||
         a == Axiom::CommuteAddMul ||
         // Both gather commutations are exact: a gather RE-INDEXES, it does not compute, and an
         // elementwise op is deterministic, so the two sides produce the same bits. `r4a` already
         // declares E0 for the unary case and is the precedent. What changes between the two sides
         // is only HOW MANY rows the op runs on — a cost question, which is exactly why these
         // belong in an e-graph rather than in a greedy pass.
         a == Axiom::GatherPushUnary || a == Axiom::GatherPushBinary;
}

// Whether the axiom is exact only once TermView has discharged a row-universal side condition.
constexpr bool axiom_needs_side_condition(Axiom a) noexcept {
  return a == Axiom::AddZero || a == Axiom::SubZero || a == Axiom::MulZero || a == Axiom::SubSelf ||
         a == Axiom::DivSelf || a == Axiom::AffineDropZeroCoef || a == Axiom::LogProduct ||
         a == Axiom::LogRecip || a == Axiom::ExpLog || a == Axiom::LogExp || a == Axiom::SqrtProduct;
}

// Whether applying the axiom is expected to REDUCE error against PRINCIPLES.md §4's oracle. Under
// the M1-M4 bit-identity contract these were failures; under §4 they are improvements extraction
// may prefer. The whole point of demoting bit-identity is in this one predicate.
constexpr bool axiom_improves_accuracy(Axiom a) noexcept {
  return a == Axiom::FmaContract || a == Axiom::MulRecipAsDiv;
}

}  // namespace epykos::rewrite
