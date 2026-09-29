// EpykosEngine — the algebraic rule interface over tape terms (P1; DESIGN ONLY; D73).
// `docs/TERM_REWRITING.md` §1 and §3.
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it.
//
// This keeps the two properties of `rewrite::Rule` (D47) that earned their keep, and drops the one
// that was an artefact of the layer:
//
//   * PURITY, KEPT. `search` and `build` are const, take a const `View`, and are total functions of
//     (view, class, binding). D62's application memo is sound ONLY because `Rule::match`/`propose`
//     are pure; handing rules a mutable graph (egg's `Applier` shape) would delete that argument.
//     The cost is that a rule cannot intern nodes itself, which is why `Expr` (term.hpp) is a data
//     description the DRIVER interns. That is the right trade and it is the single most
//     consequential difference from an off-the-shelf e-graph.
//   * SEARCH / BUILD SPLIT, KEPT. Same two-phase shape as match/propose, so accounting, logging and
//     bounds carry over.
//   * THE E0/E1 ADMISSION CLASS, DROPPED. PRINCIPLES.md §4 retired bit-identity on 2026-09-25.
//     `ErrorTerm` does not supplement an exactness class, it REPLACES it. A rule states what it
//     costs; extraction decides whether that fits. `exactness_hint()` is a bug detector and a
//     propagator shortcut, never a licence.
//
// ---------------------------------------------------------------------------------------------
// Class-uniform application: the property that makes a 517,036-node tape tractable
// ---------------------------------------------------------------------------------------------
//
// Measured (D44/M3-gate-1): the Stage A tape is **517,036 nodes** after the E0 passes, from
// 27,459,283 recorded. An e-graph seeded with half a million nodes and saturated with algebraic
// rules is not obviously tractable, and an earlier draft of this design leaned on a collapse
// argument that does not apply here — see `docs/TERM_REWRITING.md` §0.1 and §2.3, where the
// retraction is recorded.
//
// What DOES apply is that the tape is massively repetitive, which is exactly why `ir::infer` finds
// **67 domains** in those 517,036 nodes. The same deep hash that builds domains partitions tape
// nodes into a few hundred distinct SHAPES. So the driver may saturate over ONE REPRESENTATIVE per
// shape class and apply the winning rewrite to every member.
//
// `applies_class_uniformly()` is the rule's statement that this is sound for it. It is true for
// every axiom in the set below, because an axiom is a fact about a term's SHAPE. It is false for a
// rule whose side condition depends on a particular node's constants — which is why the flag
// exists rather than being assumed. Note what this is NOT: it is a driver optimisation the rule
// opts into, not a constraint the interface imposes. At the IR layer the same property was forced
// on every rule by the domain structure; here it is a choice. That inversion is the clearest
// single argument for the placement.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/algebra/error.hpp"
#include "epykos/algebra/term.hpp"

namespace epykos::algebra {

struct Match {
  ClassId site = invalid_class;
  std::vector<ClassId> bound;   // indexed by the rule's own pattern-variable numbering
  // The e-NODE of `site` that matched. The memo is keyed on the node, not the class, because
  // nodes are immutable under hash-consing while classes merge.
  std::int64_t matched_node = -1;
  bool operator==(const Match&) const = default;
};

// A sink rather than a returned vector, for one measured reason: D65's 10,111-site case must be
// STOPPABLE mid-enumeration, and a rule that returns a vector has already paid for all of it.
// `accept` returning false means "budget spent, stop searching".
class MatchSink {
 public:
  virtual ~MatchSink() = default;
  virtual bool accept(const Match& m) = 0;
};

class Rule {
 public:
  virtual ~Rule() = default;

  // "<family>.<rule>", e.g. "alg.mul_assoc", "alg.exp_product", "rec.telescope".
  virtual const std::string& name() const noexcept = 0;

  // Advisory; see error.hpp. A rule does NOT earn admission by declaring itself exact.
  virtual ExactnessHint exactness_hint() const noexcept { return ExactnessHint::Unknown; }

  // Maximum pattern depth in e-nodes, counting the site's own node as 1. Refused above
  // `Limits::max_pattern_depth` at registration.
  virtual int pattern_depth() const noexcept = 0;

  // True: a match on one member of a shape class is a match on all of them, so the driver may
  // saturate over representatives. See this header's opening. Default false is the conservative
  // direction and costs only speed.
  virtual bool applies_class_uniformly() const noexcept { return false; }

  // Advisory self-report of sites per thousand nodes; 0 means no estimate. D65 measured what
  // happens when this is wrong by an order of magnitude. The driver logs the discrepancy rather
  // than trusting it.
  virtual std::size_t opens_sites_per_kilonode() const noexcept { return 0; }

  virtual void search(const View& view, MatchSink& sink) const = 0;

  // Build the replacement for exactly one match `search` produced. Returns false when the rule
  // declines after all (a side condition the search deferred turned out to fail): legal, and NOT
  // the matcher bug an empty `Proposal` was.
  virtual bool build(const View& view, const Match& match, Rewrite& out) const = 0;
};

// --------------------------------------------------------------------------------------------
// The identity set, declared in one place
// --------------------------------------------------------------------------------------------
//
// Thirty axioms over the existing twenty ops, enumerated so a reviewer can read the list rather
// than grep for rules and a test can assert the enum and `docs/TERM_REWRITING.md` §3's table have
// not drifted apart.
//
// The three-way split is a statement about FLOATING-POINT BEHAVIOUR, not admissibility. Since §4
// retired bit-identity, an axiom in the exact bucket has no privilege over one in the inexact
// bucket. The split is kept because it is true and useful in two narrower ways: the exact ones can
// be asserted for free as bug detectors, and the conditional ones name an obligation a rule must
// discharge BEFORE it may fire, which is a soundness question and not a rounding one.
//
// Two axioms from the IR-layer draft are GONE, not deferred: `gather_push_unary` and
// `gather_push_binary`. The tape has no gathers. `r4a` remains a perfectly good IR-layer rule and
// is unaffected.
enum class Axiom : std::uint16_t {
  // --- exact in IEEE-754 double, unconditionally (a free assertion, not a licence) ---
  NegNeg,            // neg(neg(x)) = x
  SubAsAddNeg,       // sub(a,b) = add(a, neg(b))
  NegSub,            // neg(sub(a,b)) = sub(b,a)
  MulOne,            // mul(x,1) = x
  DivOne,            // div(x,1) = x
  SelectSameArms,    // select(p,x,x) = x
  SelectPushUnary,   // f(select(p,a,b)) = select(p, f(a), f(b)); D5 records both arms regardless
  CommuteAddMul,     // Add / Mul / CmpEq operand order; `cse` already canonicalises it

  // --- exact once a side condition View can discharge; the condition IS a gate ---
  AddZero,           // add(x,0) = x            REQUIRES provably_not_negative_zero
  SubZero,           // sub(x,0) = x            REQUIRES provably_not_negative_zero
  MulZero,           // mul(x,0) = 0            REQUIRES provably_finite   (inf*0 = NaN)
  SubSelf,           // sub(x,x) = 0            REQUIRES provably_finite
  DivSelf,           // div(x,x) = 1            REQUIRES provably_nonzero and provably_finite
  AffineDropZeroCoef,// drop an Affine member with c_i = 0   REQUIRES provably_finite

  // --- constant folding: the tape's own gift, keyed on Node::tainted ---
  FoldUntainted,     // an untainted sub-DAG has a value now; replace it with one Const leaf.
                     // `affine_collapse` already keys on untaintedness; this generalises it.

  // --- identities in R, not in float: each carries an ErrorTerm, and so may any above ---
  AddAssoc,          // (a+b)+c = a+(b+c)
  MulAssoc,          // (a*b)*c = a*(b*c)
  MulDistribAdd,     // a*(b+c) = a*b + a*c, and the FACTORING direction, which is the valuable one
  DivAsMulRecip,     // div(a,b) = mul(a, recip(b))     error UP
  MulRecipAsDiv,     // mul(a, recip(b)) = div(a,b)     error DOWN
  FmaContract,       // add(mul(a,b),c) = fma(a,b,c)    error DOWN
  FmaExpand,         // the reverse                      error UP
  RecipRecip,        // recip(recip(x)) = x
  ExpProduct,        // mul(exp(a),exp(b)) = exp(add(a,b))   n exps -> 1: the biggest single win
  ExpSumSegment,     // a Sum's worth of exps -> exp of their Sum
  LogProduct,        // log(a*b) = log a + log b         REQUIRES provably_positive on both
  LogRecip,          // log(recip a) = neg(log a)        REQUIRES provably_positive
  ExpLog,            // exp(log a) = a                   REQUIRES provably_positive
  LogExp,            // log(exp a) = a                   REQUIRES no overflow in exp(a)
  SqrtProduct,       // sqrt(a)*sqrt(b) = sqrt(a*b)      REQUIRES both provably non-negative
  SumFactorCommon,   // Sum(c*x_0 .. c*x_n) = c * Sum(x_0..x_n): removes n multiplies, reorders nothing

  Count_,
};

const char* to_string(Axiom a) noexcept;

constexpr bool axiom_is_unconditionally_exact(Axiom a) noexcept {
  return a == Axiom::NegNeg || a == Axiom::SubAsAddNeg || a == Axiom::NegSub || a == Axiom::MulOne ||
         a == Axiom::DivOne || a == Axiom::SelectSameArms || a == Axiom::SelectPushUnary ||
         a == Axiom::CommuteAddMul;
}

constexpr bool axiom_needs_side_condition(Axiom a) noexcept {
  return a == Axiom::AddZero || a == Axiom::SubZero || a == Axiom::MulZero || a == Axiom::SubSelf ||
         a == Axiom::DivSelf || a == Axiom::AffineDropZeroCoef || a == Axiom::LogProduct ||
         a == Axiom::LogRecip || a == Axiom::ExpLog || a == Axiom::LogExp || a == Axiom::SqrtProduct;
}

// Expected to REDUCE error against the oracle. Under the M1-M4 contract these were failures; under
// PRINCIPLES.md §4 they are improvements extraction may prefer. The whole point of demoting
// bit-identity is in this one predicate, and a test pins that it is non-empty.
constexpr bool axiom_improves_accuracy(Axiom a) noexcept {
  return a == Axiom::FmaContract || a == Axiom::MulRecipAsDiv || a == Axiom::FoldUntainted;
}

}  // namespace epykos::algebra
