// EpykosEngine — what a rewrite is, at the tape (P1 symbolic algebraic reduction; DESIGN ONLY;
// D73). `docs/TERM_REWRITING.md` §1.
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it.
//
// ---------------------------------------------------------------------------------------------
// The term graph already exists, and it is the tape
// ---------------------------------------------------------------------------------------------
//
// `tape/tape.hpp`: "A Tape is a straight-line program in topological order: node i only reads
// nodes < i. Constants are leaves, one node per distinct bit pattern. Variadic operands live in a
// side array." That is a term DAG. It is already hash-consed — `cse` merges nodes with the same
// op, operands (commutative operands in canonical order) and constant bit pattern — and
// `tape/passes.cpp` already rewrites it in place. `affine_collapse` is already an algebraic
// simplification on it, and per `docs/PRIOR_ART.md` it is what recovers SwapEngine's hand-built
// W-cache exactly, 857 rows.
//
// So a TERM is a node, and the sub-DAG rooted at it. There is nothing to invent:
//
//     TermRef == node_id
//
// The new stage sits between the existing passes and inference:
//
//     record -> tape passes -> SYMBOLIC ALGEBRAIC REDUCTION -> ir::infer -> plan -> execute
//
// Nothing below it changes. Inference, planning and execution receive a smaller tape and run
// exactly as they do now, which is the strongest property this placement has: the blast radius of
// the entire package is one call site.
//
// ---------------------------------------------------------------------------------------------
// What this layer does NOT have to carry, and the earlier draft did
// ---------------------------------------------------------------------------------------------
//
// An earlier version of this design targeted `ir::Program` and had to carry three complications
// that simply do not exist here. Recorded because the contrast is the argument for the placement
// (`docs/TERM_REWRITING.md` §0.1):
//
//   * NO ANCHOR DOMAIN. An `ir::Program` step is one op applied to every row of a domain, so a
//     term there is a row-indexed VECTOR and two structurally identical terms in two domains
//     denote different values. A tape node is one scalar. The whole anchor apparatus goes.
//   * NO ROW-UNIVERSALITY, AND NO ROW-SUBSET QUESTION. "Can a rule rewrite a subset of rows?" was
//     the hard question at the IR layer and is meaningless here: a node is one value, and
//     rewriting it rewrites exactly that value. What was a CONSTRAINT becomes, at most, a driver
//     OPTIMISATION — see `rule.hpp`'s note on class-uniform application.
//   * NO INDEX MAPS. A `Gather` is a per-row value id, so crossing a domain boundary at the IR
//     layer meant composing with an index map and commuting rules across it. The tape has no
//     gathers; an operand is a node id. `r4a.push_unary_through_gathers` has no analogue and needs
//     none.
//
// What the tape DOES have that the IR layer does not: `Node::tainted`, computed at record time —
// "depends on an Input". An untainted sub-DAG is a compile-time constant and may be FOLDED at
// rewrite time, which is a capability, not an obligation. `affine_collapse` already keys on it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epykos/algebra/error.hpp"
#include "epykos/tape/op.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::algebra {

// A term is a node and the sub-DAG under it. This alias exists to say so once.
using TermRef = node_id;

// A term's e-class.
using ClassId = std::int64_t;
inline constexpr ClassId invalid_class = -1;

// --------------------------------------------------------------------------------------------
// A replacement expression
// --------------------------------------------------------------------------------------------

enum class ExprRefKind : std::uint8_t {
  // A node of this same Expr's arena: `index` into Expr::nodes.
  Node = 0,
  // An e-class the match bound — a pattern hole. `index` into Match::bound. This is how a rewrite
  // REUSES structure instead of copying it: `mul(a, add(b,c)) -> add(mul(a,b), mul(a,c))` names
  // a, b and c three times and materialises nothing.
  Bound = 1,
  // An existing tape node, verbatim.
  Existing = 2,
  // A new constant. `konst` carries the value; the driver interns it through the tape's own
  // one-node-per-bit-pattern rule, so +0.0 and -0.0 stay distinct exactly as `Tape::constant`
  // makes them.
  Constant = 3,
};

struct ExprRef {
  ExprRefKind kind = ExprRefKind::Node;
  std::int32_t index = -1;   // Node / Bound
  node_id existing = invalid_node;  // Existing
  double konst = 0.0;        // Constant
  bool operator==(const ExprRef&) const = default;

  static ExprRef node(std::int32_t i) noexcept { return ExprRef{ExprRefKind::Node, i, invalid_node, 0.0}; }
  static ExprRef bound(std::int32_t i) noexcept { return ExprRef{ExprRefKind::Bound, i, invalid_node, 0.0}; }
  static ExprRef existing_node(node_id n) noexcept { return ExprRef{ExprRefKind::Existing, -1, n, 0.0}; }
  static ExprRef constant(double v) noexcept { return ExprRef{ExprRefKind::Constant, -1, invalid_node, v}; }
};

// One constructed node. Mirrors `Node`'s shape so lowering is mechanical. Variadic ops carry their
// operands in `args` (and `coefs` for Affine) rather than the tape's side-array offsets, which the
// driver assigns when it interns.
struct ExprNode {
  Op op = Op::Const;
  ExprRef a{}, b{}, c{};
  double konst = 0.0;              // Const value / Affine c_0
  std::vector<ExprRef> args;       // Sum, Affine
  std::vector<double> coefs;       // Affine, parallel to args
};

// A replacement, standing on its own: PURE DATA. A rule builds one and hands it back; the DRIVER
// interns it. Rules never touch the graph — see rule.hpp for why that is not an aesthetic choice.
struct Expr {
  std::vector<ExprNode> nodes;
  ExprRef root{};
  bool empty() const noexcept { return nodes.empty() && root.index < 0 && root.kind == ExprRefKind::Node; }
};

// --------------------------------------------------------------------------------------------
// What a rewrite IS
// --------------------------------------------------------------------------------------------

// An EQUALITY CLAIM, not an instruction: the driver unions `site` with `replacement` and keeps
// both forms. Extraction chooses later.
//
// Contrast `rewrite::Proposal`, which holds an optional whole `ir::Program`. That is not a flaw
// being corrected — it is correct for the layer it sits at, where `ir::infer` has already grouped
// structurally identical nodes so that a step applies to every row at once and there are no terms
// left to rewrite, only arrays. `Proposal` stays exactly as it is. This is a different stage.
struct Rewrite {
  ClassId site = invalid_class;
  Expr replacement;

  // ADVISORY (error.hpp's ExactnessHint). PRINCIPLES.md §4 retired bit-identity as a contract on
  // 2026-09-25, so a rewrite is never admissible BECAUSE it preserves bits. `Unknown` is the
  // honest default and costs only a propagator call.
  ExactnessHint exactness = ExactnessHint::Unknown;

  // The local error this rewrite introduces — the ONLY input to admissibility.
  ErrorTerm error{};

  // Why it is sound, in a form a reviewer and a test can both read: an axiom name ("mul.assoc"),
  // or, for a rule whose soundness is a proof rather than a pattern (recurrence.hpp), the name of
  // the discharged obligation. Carried into the extracted candidate's history, so a surprising
  // result can be traced to the fact that licensed it.
  std::string justification;
};

// --------------------------------------------------------------------------------------------
// The read-only view a rule sees
// --------------------------------------------------------------------------------------------

// One e-node: an op over child e-CLASSES, or a leaf.
struct NodeView {
  Op op = Op::Const;
  ClassId a = invalid_class, b = invalid_class, c = invalid_class;
  double konst = 0.0;
  std::vector<ClassId> args;   // Sum, Affine
  std::vector<double> coefs;   // Affine
  std::int32_t input_ordinal = -1;  // Op::Input only
};

// Everything a Rule may read. An abstract interface rather than the graph type, so a rule can be
// unit-tested against a hand-built view.
class View {
 public:
  virtual ~View() = default;

  // The tape these terms were read from. A rule reads `Node::konst` and `Node::tainted` to
  // discharge side conditions EXACTLY — never to evaluate anything the tape has not already.
  virtual const Tape& tape() const noexcept = 0;

  virtual std::int64_t num_classes() const noexcept = 0;
  // The e-nodes of one class, deterministically ordered. A class holds several nodes precisely
  // when the graph has learned several equal forms; a rule matches AGAINST NODES and binds
  // CLASSES.
  virtual std::vector<NodeView> nodes_of(ClassId c) const = 0;
  virtual ClassId class_of(node_id n) const noexcept = 0;

  // True when no node of this class depends on an Input — `Node::tainted` lifted to a class. Such
  // a term has a value NOW, and `constant_value` returns it. This is the tape's own gift to an
  // algebraic stage and has no analogue at the IR layer.
  virtual bool untainted(ClassId c) const = 0;
  virtual bool constant_value(ClassId c, double& out) const = 0;

  // ---- side conditions, discharged from Const leaves and op ranges ---------------------------
  // False means "not shown", never "false". Weaker here than the IR layer's Column scan — there
  // is no array of values to inspect, only structure and constants — which is a real cost of the
  // placement and is recorded in `docs/TERM_REWRITING.md` §1.4.
  virtual bool provably_positive(ClassId c) const = 0;
  virtual bool provably_nonzero(ClassId c) const = 0;
  virtual bool provably_finite(ClassId c) const = 0;
  // `add(x,0) = x` fails only at x = -0, and the engine can observe it through Recip, Div and
  // Sqrt. Comparisons cannot (-0 == 0), which is why the case is easy to miss.
  virtual bool provably_not_negative_zero(ClassId c) const = 0;

  // ---- how many times this shape occurs -------------------------------------------------------
  // The structural class this node belongs to under `ir::analysis`'s deep hash — the same
  // equivalence `ir::infer` uses to build domains. A rewrite found on one member applies to every
  // member, which is what makes a 517,036-node tape tractable (§2.3). -1 when unknown.
  virtual std::int32_t shape_class_of(node_id n) const noexcept = 0;
  virtual std::int32_t shape_class_size(std::int32_t shape_class) const noexcept = 0;
};

}  // namespace epykos::algebra
