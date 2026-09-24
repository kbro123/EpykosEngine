// EpykosEngine — the catalogue's canonical group signature (M4/C1; PROBLEM.md §7, DESIGN.md §7
// tier 1, D1).
//
// DESIGN.md §7: "Each fused group has a canonical signature. A build-time tool runs the reference
// workloads, collects hot signatures, emits C++, and compiles it into the engine." A "fused
// group" here is a domain's `ir::Group` (the op sequence the signature pass — ir/signature.hpp,
// DESIGN.md §5 — already produced): the natural unit exec::Interpreter and adjoint::Adjoint
// already evaluate one row at a time. A Signature is a fingerprint of that op sequence that is
// blind to everything DATA-dependent — row count, literal/column/gather VALUES, which
// Program-global table entry a slot happens to read — and sensitive only to the SHAPE the maths
// takes: the op at each step and, per operand slot, which KIND of thing it reads (an earlier
// step, a literal, a column, a gather, or — for the group's Sum/Affine terminal step only — a
// segment). Two domains with the same op sequence over the same slot-kind pattern get the same
// Signature, whatever their row counts, whatever curve or trade produced them, in the SAME
// Program or in two different ones from two different problem instances (HARD RULE 9: this
// package's rules are keyed on IR structure, never on an instance count or a magic constant —
// the gate "a second seed of the Stage A problem hits the same kernels" is exactly this property,
// checked directly in tests/exec/interpreter_catalogue_e0_test.cpp's
// DifferentStageAInstanceHitsTheSameCatalogueAndIsMostlyCovered and
// tests/adjoint/adjoint_catalogue_e0_test.cpp's SmallStageAIncludingScanDomains; the
// platform-independence half of it, added by D61, is
// tests/catalogue/signature_commutative_e0_test.cpp).
//
// What is deliberately NOT part of a Signature: `ir::Domain::rows` (row count), any
// `ir::Program::literals` / `columns` / `gathers` VALUE, which literal/column/gather/segment
// TABLE ENTRY a slot names (only its KIND), the domain's id, name or level, whether the
// domain happens to be a scan (`ir::Domain::recurrent` / `scan_class`) — a scan's carry is
// recorded as an ordinary Gather slot like any other; row-order sequencing is a property of HOW
// a kernel is CALLED (one row at a time, in ascending row order — kernel.hpp), not of the group's
// shape, so it does not belong in the fingerprint the generator and the two engines' dispatch
// both key on — and, since D61, the ORDER OF A COMMUTATIVE STEP'S TWO OPERANDS.
//
// Commutative canonicalisation (D61). `ir::infer`'s own signature pass already treats `a op b`
// and `b op a` as ONE isomorphism class for a commutative op (`op_is_commutative`: Add, Mul,
// CmpEq — its class hash sorts the two operand tokens, ir/signature.cpp `hash_all` /
// `extract_tree`). Which of the two orders the emitted `ir::Step` then carries is NOT canonical:
// `Class::emit_swapped` reproduces "the first instance's recorded operand order", i.e. whichever
// order the FIRST tape node of that class happened to be recorded in. Tape order is not stable
// across compilers — C++ leaves the evaluation order of a binary operator's operands unsequenced,
// so two sibling subexpressions of one recorded statement are appended to the tape in whichever
// order the compiler chose (D25 saw the same thing break `tape_replay_e0_test`). Measured on the
// default Stage A tape: GCC 13 and Apple clang record the SAME 517,036 nodes with the same
// per-node operand slots, in a different order, and `ir::infer` therefore emits nine domains as
// `mul(lit, gat)` / `mul(col, gat)` under one compiler and `mul(gat, lit)` / `mul(gat, col)`
// under the other. A fingerprint that called those different shapes made the catalogue
// compiler-dependent (63/66 groups on GCC vs 66/66 on clang; D61). `signature_of` therefore
// orders a commutative step's `a` / `b` by (SlotShape, back) — `canonical_swap_ab` below — and
// `bind_domain` (kernel.cpp) and the generator (tools/catalogue/codegen.cpp) walk the same
// canonical order, so the one generated kernel serves both recordings. E0: IEEE-754 `+` and `*`
// are exactly commutative, so evaluating the canonical order is bit-for-bit the generic path's
// result. A fixed-arity `Sum` is NOT touched: `op_is_commutative(Op::Sum)` is false and its fold
// is a left fold whose reassociation would change rounding (DESIGN.md §5.6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"

namespace epykos::catalogue {

// The kind of thing one operand slot of a catalogued step reads. Never the concrete table index
// (ir::Slot::index) — that is Program-instance data, exactly the row counts and literal/column/
// gather values this fingerprint excludes by design (see file header).
enum class SlotShape : std::uint8_t {
  None = 0,     // operand not used by this op
  Step = 1,     // an earlier step of the same group
  Literal = 2,  // Program::literals: one double, shared by every row
  Column = 3,   // Program::columns: one double per row of this domain
  Gather = 4,   // Program::gathers: one value id per row (may read this domain's own earlier
                // rows — a scan's carry — or any other domain; the shape does not distinguish
                // the two, see file header)
  Segment = 5,  // Program::segments: the group's Sum/Affine terminal step only
};

const char* to_string(SlotShape s) noexcept;

// One step's shape: the op, plus its operand slots' kinds (a, b, c, konst — ir::Step's own
// field order, except that a commutative step's a and b are presented in canonical order; see
// the file header and `canonical_swap_ab`). A fixed-arity Sum (`ir::is_fixed_sum`, scan groups: a Sum whose members are
// ordinary operands, not a Segment — DESIGN.md §5.6) reports its members' true kinds (Step,
// Literal, Column or Gather), exactly like any other 2- or 3-ary op: it IS one, this fingerprint
// does not special-case it. A segment-fold Sum or Affine (the group's LAST step only,
// `ir::is_whole_segment`) reports `a` as Segment; Affine's `konst` is Literal or Column, Sum's is
// always None (Sum has no leading constant). Arities fall out of `op` plus which slots are not
// `None` — never stored separately.
//
// `a_back` / `b_back` / `c_back` / `konst_back`: for a slot whose shape is Step, how many steps
// back it points — this step's own index minus the referenced step's (always >= 1: `ir::Group`
// is topological, so a Step operand only ever names an EARLIER step). This is genuine, value-
// independent STRUCTURE — which earlier result an op combines, not a row count or a table entry
// — so two steps that both read "an earlier step" but a DIFFERENT one are different shapes, not
// the same one: without it, a generated kernel would not know which of several already-computed
// step values to plug into a later expression, and two op sequences that only coincide in "some
// step depends on some earlier step" could wrongly collide onto one Signature. Meaningless (left
// at 0) when the matching slot's shape is not Step.
struct StepShape {
  Op op = Op::Const;
  SlotShape a = SlotShape::None;
  SlotShape b = SlotShape::None;
  SlotShape c = SlotShape::None;
  SlotShape konst = SlotShape::None;
  std::int32_t a_back = 0;
  std::int32_t b_back = 0;
  std::int32_t c_back = 0;
  std::int32_t konst_back = 0;

  bool operator==(const StepShape&) const = default;
};

// A canonical, value- and row-count-independent fingerprint of one domain's Group: see the file
// header for exactly what it does and does not capture.
struct Signature {
  std::vector<StepShape> steps;  // one entry per ir::Group::steps, in order

  bool operator==(const Signature&) const = default;

  // A 64-bit content hash (FNV-1a over the step shapes), used by the registry to order its table
  // for lookup. Never trusted alone: registry::lookup compares the full Signature on every
  // candidate a hash match names, so a collision can never dispatch the wrong kernel. (D55's own
  // header named a mutant `catalogue.hash_collision_returns_wrong_kernel` and a
  // tests/catalogue/registry_test.cpp for this; neither was ever written — corrected here rather
  // than left standing, D61. The generator's own collection map, tools/catalogue/
  // generate_main.cpp, keys on this hash ALONE without a full-equality re-check, so a collision
  // there would silently drop one of the two signatures: lost coverage, never a wrong kernel.)
  std::uint64_t hash() const noexcept;

  // A short, deterministic, human-readable rendering, e.g. "neg(gat);mul(step,lit);exp(step)" —
  // used to name the generated kernel function and to make a registry dump or a coverage report
  // legible. Stable across runs of the generator (iteration order of `steps` only).
  std::string to_string() const;
};

// Whether domain `d` of `program` is eligible for a Signature / catalogued kernel at all:
//   - the domain has at least one step (ir::validate already rejects an empty group);
//   - no step is Op::Input (an Input step reads the run's `state` array by ordinal, which the
//     catalogue kernel ABI — kernel.hpp — does not carry: an Input domain is always the tape's
//     own free-input rows, never a "hot group" DESIGN.md §7 means to catalogue, so it is simply
//     out of scope rather than a special case of anything else);
//   - every step's op is one `ir::op_is_supported` accepts (defensive: ir::validate already
//     enforces this for the whole Program).
// A domain that fails this is never handed to `signature_of`; each engine applies its OWN
// additional eligibility on top (exec::Interpreter also requires the plan's Materialise choice
// and excludes its own already-optimised whole-segment kernel; adjoint::Adjoint has no further
// restriction — see each header for why).
bool is_cataloguable(const ir::Program& program, ir::domain_id d) noexcept;

// Whether the canonical form of `step` (step index `k` of its group) presents `b` before `a`
// (D61, file header). True only for a commutative op (`op_is_commutative`: Add, Mul, CmpEq) with
// both operands present whose `b` slot sorts before its `a` slot on the key (SlotShape, steps
// back) — a purely structural key, never a table index or a value. Every consumer that walks a
// step's operand slots in first-use order must use it, or the k-th Literal / Column / Gather
// OCCURRENCE a generated kernel reads would not be the k-th one `bind_domain` bound: that is
// `signature_of` (below), `bind_domain` (kernel.hpp) and `tools/catalogue/codegen.cpp`.
bool canonical_swap_ab(const ir::Step& step, std::size_t k);

// The signature of domain `d`. Precondition: is_cataloguable(program, d) (checked with an
// assertion in debug builds; UB-free but meaningless otherwise — a caller that has not checked
// eligibility is a bug in that caller, not a condition this function is asked to detect, exactly
// rewrite::Rule::propose's own contract, rewrite/rule.hpp).
Signature signature_of(const ir::Program& program, ir::domain_id d);

}  // namespace epykos::catalogue
