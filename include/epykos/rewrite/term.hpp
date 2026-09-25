// EpykosEngine — the term language a rewrite replaces (P1/term-rewriting DESIGN ONLY; D73).
//
// STATUS: this header is an INTERFACE PROPOSAL. Nothing implements it. It exists so the shape can
// be reviewed before roughly six thousand lines are written against it. `docs/TERM_REWRITING.md`
// is the design it belongs to and is the document to read first; `docs/PRINCIPLES.md` §6 item 1
// is the blocker it removes.
//
// ---------------------------------------------------------------------------------------------
// What a term IS, in this IR
// ---------------------------------------------------------------------------------------------
//
// The domain IR is not a term DAG. It is `domains` (ir/program.hpp), each with a `Group`: a short
// list of `Step`s evaluated once per ROW of the domain. A Step's operands are `Slot`s — an earlier
// Step of the same group, a Literal (uniform over rows), a Column (one double per row), a Gather
// (one value id per row), a Segment (a per-row variadic member list), or an Input.
//
// So a group's steps already form a small DAG, and:
//
//   A TERM is the sub-DAG rooted at one Step of one Group. It denotes, for each row r of that
//   Group's domain, one scalar: the value that Step takes at row r. The domain is the term's
//   ANCHOR: a term is a ROW-INDEXED VECTOR of scalars, not a scalar.
//
// Three consequences, all of which this interface has to carry and none of which the
// whole-`ir::Program` `rewrite::Proposal` could:
//
// 1. A TERM REWRITE APPLIES TO EVERY ROW OF ITS ANCHOR DOMAIN. That is not a restriction added
//    here; it is what a domain IS (`ir::infer`'s isomorphism class: `rows` instances of ONE op
//    sequence). Rewriting "step 3 of group 7" rewrites it for all 16,103 rows at once, which is
//    exactly why a term rewrite is cheap where a whole-Program Proposal is not.
//
// 2. A RULE CANNOT REWRITE A SUBSET OF ROWS. There is no row predicate anywhere in this header
//    and that is deliberate: a per-row conditional rewrite would turn one group into a branchy
//    one and destroy the property the executor is built on. A rewrite valid for only some rows
//    must FIRST split the domain so those rows become their own domain — which is
//    `r2.bucket_rows`' job, a PROGRAM-tier rule, not a term rule. Row-subset rewriting is
//    therefore expressible as (program-tier split) then (term-tier rewrite), never as one term
//    rewrite. `docs/TERM_REWRITING.md` §1.3.
//
// 3. SIDE CONDITIONS ARE ROW-UNIVERSAL, AND THE IR CAN USUALLY DISCHARGE THEM EXACTLY. "x >= 0
//    for every row" is not a numerical hope: when x is a Column it is a `std::vector<double>`
//    sitting in the Program and the condition is a scan of it. This makes a class of conditional
//    identities (log(a*b) = log a + log b, sqrt(a)*sqrt(b) = sqrt(a*b)) available as PROVED
//    rewrites rather than guessed ones — at the price that the proof is about THIS RECORDING,
//    which is what a recording compiler is entitled to assume (D47's own framing).
//
// ---------------------------------------------------------------------------------------------
// Crossing a domain boundary: the index map is part of the term
// ---------------------------------------------------------------------------------------------
//
// A Gather slot names one value id per row, and a value id is (domain, row) globally. A row's
// value is its group's LAST step (program.hpp's evaluation contract), so a gather into domain p
// always denotes "the ROOT term of group p, re-indexed by this gather's index vector". Writing
// that composition down is what lets algebra cross a domain boundary at all:
//
//     Gath(g, t)      the term t of the producing domain, observed at the rows g names
//
// Congruence handles `Gath(g, t) == Gath(g, t')` when `t == t'` and g is the same gather. It does
// NOT handle `Gath(g, f(t)) == f(Gath(g, t))`, which is a genuine mathematical fact about
// elementwise f and an index map — that is a RULE (`r4a.push_unary_through_gathers` generalised),
// and it is the bridge every cross-domain identity goes over. `docs/TERM_REWRITING.md` §2.4 for
// why the distinction is load-bearing, §6.4 for why it is the main explosion risk.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/rewrite/error_model.hpp"
#include "epykos/rewrite/rule.hpp"
#include "epykos/tape/op.hpp"

namespace epykos::rewrite {

// --------------------------------------------------------------------------------------------
// Addressing a term
// --------------------------------------------------------------------------------------------

// The term rooted at `step` of `program.groups[domain]`. This is the whole of a site's identity:
// a group's steps are topological, so a step index names exactly one sub-DAG.
struct TermRef {
  ir::domain_id domain = -1;
  std::int32_t step = -1;
  bool operator==(const TermRef&) const = default;

  bool valid() const noexcept { return domain >= 0 && step >= 0; }
};

// A term's e-class. Distinct from `optimise::ClassId`, the PROGRAM tier's, which this design
// keeps for the layout rules (`docs/TERM_REWRITING.md` §7).
using TermClassId = std::int64_t;
inline constexpr TermClassId invalid_term_class = -1;

// --------------------------------------------------------------------------------------------
// A replacement expression
// --------------------------------------------------------------------------------------------

// What one operand of a replacement node points at.
enum class ExprRefKind : std::uint8_t {
  // A node of this same TermExpr's arena: `index` into TermExpr::nodes.
  Node = 0,
  // An e-class the match bound — a pattern "hole". `index` into TermMatch::bound. This is how a
  // rewrite REUSES structure instead of copying it, and it is the whole difference from
  // Proposal: `mul(a, add(b, c)) -> add(mul(a,b), mul(a,c))` names a, b and c three times and
  // materialises nothing.
  Bound = 1,
  // A Slot of the anchor group, verbatim (`slot` carries it): a leaf the rewrite does not touch.
  Slot = 2,
  // New DATA the rewrite introduces: `index` into the matching TermExpr::new_* vector. A closed
  // form usually needs this — the arithmetic-series collapse needs a per-row step index k, which
  // is a Column that does not exist in the program yet. A rewrite that could only rearrange
  // existing nodes could not express any of PRINCIPLES.md §2a's closed forms except the telescope.
  NewLiteral = 3,
  NewColumn = 4,
  NewGather = 5,
  NewSegment = 6,
};

struct ExprRef {
  ExprRefKind kind = ExprRefKind::Node;
  std::int32_t index = -1;
  ir::Slot slot{};  // ExprRefKind::Slot only
  bool operator==(const ExprRef&) const = default;

  static ExprRef node(std::int32_t i) noexcept { return ExprRef{ExprRefKind::Node, i, {}}; }
  static ExprRef bound(std::int32_t i) noexcept { return ExprRef{ExprRefKind::Bound, i, {}}; }
  static ExprRef of_slot(ir::Slot s) noexcept { return ExprRef{ExprRefKind::Slot, -1, s}; }
  static ExprRef new_literal(std::int32_t i) noexcept { return ExprRef{ExprRefKind::NewLiteral, i, {}}; }
  static ExprRef new_column(std::int32_t i) noexcept { return ExprRef{ExprRefKind::NewColumn, i, {}}; }
};

// One constructed node of a replacement. Mirrors ir::Step's shape so lowering is mechanical.
struct ExprNode {
  Op op = Op::Const;
  ExprRef a{}, b{}, c{};
  ExprRef konst{};  // Affine c_0 / Const value
  bool operator==(const ExprNode&) const = default;
};

// A replacement term, standing on its own: a small arena plus whatever new data tables it needs.
// It is PURE DATA. A rule builds one and hands it back; the DRIVER interns it into the graph.
// Rules never touch the graph — see term_rule.hpp for why that is not an aesthetic choice but the
// precondition for D62's application memo staying sound.
struct TermExpr {
  std::vector<ExprNode> nodes;
  ExprRef root{};

  // New data the replacement introduces. `new_columns[i]` must have exactly one entry per row of
  // the anchor domain; `new_gathers[i]` likewise, holding value ids legal at the anchor domain's
  // position (ir::validate's no-forward-reads rule still applies and the driver checks it). A
  // rewrite needing a value from a LATER domain is rejected, not reordered: reordering domains is
  // a program-tier edit.
  std::vector<double> new_literals;
  std::vector<std::vector<double>> new_columns;
  std::vector<std::vector<ir::value_id>> new_gathers;
  std::vector<ir::Segment> new_segments;

  bool empty() const noexcept { return nodes.empty() && root.index < 0 && root.kind == ExprRefKind::Node; }
};

// --------------------------------------------------------------------------------------------
// What replaces rewrite::Proposal
// --------------------------------------------------------------------------------------------

// One rule application. Contrast `rewrite::Proposal`, which held an `std::optional<ir::Program>`:
// every match materialised, validated and serialised an ENTIRE Program, so an identity matching
// in a thousand places cost a thousand Programs (PRINCIPLES.md §6 item 1; measured, D65: R2 at
// 10,111 site-matches blew the node bound in two rounds after 700 s). A TermRewrite is tens of
// bytes and names a single e-class.
//
// `site_class` and `replacement` are an EQUALITY CLAIM, not an instruction: the driver UNIONS
// them and keeps both forms, exactly as D49 intended and rule.hpp point 2 already described for
// the program tier. Extraction chooses later.
struct TermRewrite {
  TermClassId site_class = invalid_term_class;
  TermExpr replacement;

  // ADVISORY ONLY (error_model.hpp's `ExactnessHint`). PRINCIPLES.md §4 retired bit-identity as a
  // contract on 2026-09-25, so a rewrite is never admissible BECAUSE it preserves bits. This
  // field buys two things and neither is a licence: a free assertion that is a good bug detector
  // where exactness costs nothing to check, and a shortcut past the propagator when every
  // contribution is exact. `Unknown` is the honest default and costs only a propagator call.
  //
  // Note what `Exact` would have to mean here even if it were a gate: bit-identical for EVERY ROW
  // OF EVERY LANE, which over a domain of 16,103 rows is a far stronger claim than over one
  // scalar. That it was ever the default admission criterion is the thing §4 fixed.
  ExactnessHint exactness = ExactnessHint::Unknown;

  // The local error this rewrite introduces at its own site (error_model.hpp) — the ONLY input to
  // admissibility. Extraction composes it against the output class's budget. It is not itself a
  // gate: the gate is PRINCIPLES.md §4's oracle, run once on the extracted candidate.
  ErrorTerm error{};

  // Why this rewrite is sound, in a form a reviewer and a test can both read: an axiom name
  // ("mul.assoc"), or, for a rule whose soundness is a proof rather than a pattern
  // (recurrence.hpp), the name of the discharged proof obligation. Carried into the extracted
  // candidate's history, so a surprising extraction can be traced to the fact that licensed it.
  std::string justification;
};

// --------------------------------------------------------------------------------------------
// The read-only view a rule sees
// --------------------------------------------------------------------------------------------

// One e-node: an op over child e-CLASSES, or a leaf. Two e-nodes with equal content are the SAME
// e-node (hash-consing), so "match only a class representative" — half of D62's mechanism (A) —
// stops being an optimisation and becomes true by construction. `docs/TERM_REWRITING.md` §2.3.
struct TermNodeView {
  Op op = Op::Const;
  // Children as e-classes; `invalid_term_class` where the op does not use that operand.
  TermClassId a = invalid_term_class, b = invalid_term_class, c = invalid_term_class;
  // Leaf payload when the node IS a leaf (Literal / Column / Gather / Segment / Input), or an
  // Affine's konst. `ir::SlotKind::None` otherwise.
  ir::Slot leaf{};
  // The domain whose rows index this node's value. PART OF THE NODE'S IDENTITY: two structurally
  // identical terms anchored in different domains denote different vectors and must NOT be
  // unioned. `docs/TERM_REWRITING.md` §2.2.
  ir::domain_id anchor = -1;
};

// Everything a TermRule may read. An abstract interface rather than the graph type itself, so
// `rewrite/` need not include `optimise/` (the program tier already depends the other way,
// egraph.hpp -> rule.hpp) and so a rule can be unit-tested against a hand-built view.
class TermView {
 public:
  virtual ~TermView() = default;

  // The program these terms were read from. A rule reads its Column / Literal / Gather / Segment
  // tables to DISCHARGE SIDE CONDITIONS EXACTLY (this header's point 3) — never to evaluate.
  virtual const ir::Program& program() const noexcept = 0;

  virtual std::int64_t num_classes() const noexcept = 0;
  // The e-nodes of one class, in a deterministic order. A class holds several nodes precisely
  // when the graph has learned several equal forms; a rule matches AGAINST NODES and binds
  // CLASSES.
  virtual std::vector<TermNodeView> nodes_of(TermClassId c) const = 0;
  virtual ir::domain_id anchor_of(TermClassId c) const noexcept = 0;
  // The class of the term rooted at `ref`, or `invalid_term_class` when it is not interned.
  virtual TermClassId class_of(TermRef ref) const noexcept = 0;

  // A class whose value is the same uniform double in every row, when the graph can SHOW it (a
  // Literal leaf, or an op over such). False otherwise — never "probably".
  virtual bool uniform_value(TermClassId c, double& out) const = 0;

  // ---- side-condition support: exact scans of data already in the Program --------------------
  // True when every row of `c`'s value is provably > 0 / != 0 / finite, discharged from Column
  // and Literal contents and from op ranges (exp is positive, x*x is non-negative). A rule
  // needing a condition this cannot discharge must not fire. False means "not shown", never
  // "false".
  virtual bool provably_positive(TermClassId c) const = 0;
  virtual bool provably_nonzero(TermClassId c) const = 0;
  virtual bool provably_finite(TermClassId c) const = 0;
  // True when no row of `c` can be a negative zero. `x + 0 -> x` is E0 for every double EXCEPT
  // x = -0 (which becomes +0), and the engine CAN observe that difference, through Recip, Div
  // and Sqrt — so the axiom carries the obligation rather than quietly ignoring it.
  virtual bool provably_not_negative_zero(TermClassId c) const = 0;
};

}  // namespace epykos::rewrite
