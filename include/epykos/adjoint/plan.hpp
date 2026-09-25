// EpykosEngine — the adjoint plan: the reverse of a domain-IR Program, derived at build time
// (DESIGN.md §3 "adjoint" column, §7 "Adjoints"; M2/Q3). Plain data, no pointers: the local
// rule of every op, the transpose of every index array as CSR "who reads me" lists, and the slot
// layout of the per-edge adjoint buffers. adjoint.hpp's Adjoint runs it.
//
// The reverse of a Program, group by group in reverse evaluation order. Every value v of the
// value space gets an adjoint v̄ = ∂(out_bar · outputs)/∂v. The reverse of a group takes the
// row's v̄ (the adjoint of its last step), runs the local rule of each step from the last to the
// first — an operand that is an earlier step accumulates into that step's adjoint, an operand
// that is a gather accumulates into the *edge slot* of that (gather, row), an operand that is a
// literal or a column receives nothing — and, for a Sum / Affine step, stores the step's adjoint
// in the edge slot of that (segment, row). Nothing is ever scattered: a value's adjoint is later
// PULLED by its own domain as the sum of the edge slots that read it (DESIGN.md §7),
//
//   v̄ = Σ_{o : outputs[o] = v} out_bar[o]                      (seeds, ordinal order)
//      + Σ_{(g, r) : gathers[g].index[r] = v} gbar[g, r]          (gather readers)
//      + Σ_{(s, r, m) : Sum segment s, members[m] = v} segbar[s, r]              (Sum: broadcast)
//      + Σ_{(s, r, m) : Affine segment s, members[m] = v} coefs[m] · segbar[s, r]  (Affine: Wᵀ),
//
// summed left to right from +0.0 in exactly that order (ordinals ascending; gathers in Program
// order then rows ascending; segments in Program order then rows then members) — the "who reads
// me" CSR lists below, built once. An Affine's reader list is the transpose of its coefficient
// table: on a curve that is the calibration Jacobian shape Wᵀ (DESIGN.md §7). The Input domain's
// v̄ is the state adjoint. A scan domain (ir::Program::scans, D41) needs no extra table: the
// carry is a gather whose edge slot (carry, row r + 1) is one of row r's readers, so the reverse
// scan is the same pull applied to the rows backwards, one row at a time (adjoint.hpp). A
// recurrent domain that is not a scan is refused.
//
// Materialisation (D31): the forward pass stores every row value (the last step of every group,
// the same value buffer layout as exec::Interpreter, batch innermost). Intermediate steps of a
// group are not stored: the reverse recomputes steps 0 .. last-1 of a tile from the stored
// operands (gathers of earlier domains, columns, literals) and reads the last step from the
// value buffer, so an Exp / Log / Sqrt that is a group's value (every M1 exp) is never
// recomputed and the arithmetic intermediates are recomputed with the forward's bits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/tape/op.hpp"

namespace epykos::adjoint {

// The local rule of an op y = op(a, b, c) with ȳ the adjoint of y: which operands receive an
// adjoint and the formula (as the runtime evaluates it, roundings included; `+=` accumulates).
struct OpRule {
  bool a = false;
  bool b = false;
  bool c = false;
  const char* formula = "";
};
// Reserved ops (after Affine) report no operands and formula "unsupported".
OpRule adjoint_rule(Op op) noexcept;

struct AdjointPlan {
  // Edge slots. (gather g, row r) has slot gather_slot_base[g] + r in the gather-adjoint buffer;
  // (segment s, row r) has slot segment_slot_base[s] + r in the segment-adjoint buffer. A buffer
  // holds one double per slot per lane, batch innermost: slot·L + l.
  std::vector<std::int32_t> gather_slot_base;   // per Program::gathers entry
  std::vector<std::int32_t> segment_slot_base;  // per Program::segments entry
  std::int32_t n_gather_slots = 0;
  std::int32_t n_segment_slots = 0;
  std::vector<std::uint8_t> segment_is_affine;  // per segment: read by an Affine step (else Sum)

  // "Who reads me": four CSR lists over value ids (offsets have num_values + 1 entries), in the
  // accumulation order documented above.
  std::vector<std::int32_t> output_offsets;
  std::vector<std::int32_t> output_readers;   // output ordinals
  std::vector<std::int32_t> gather_offsets;
  std::vector<std::int32_t> gather_readers;   // gather edge slots
  std::vector<std::int32_t> sum_offsets;
  std::vector<std::int32_t> sum_readers;      // segment edge slots of Sum steps
  std::vector<std::int32_t> affine_offsets;
  std::vector<std::int32_t> affine_readers;   // segment edge slots of Affine steps
  std::vector<double> affine_coefs;           // parallel to affine_readers: the member's coefficient

  struct DomainPlan {
    std::vector<std::int32_t> gathers;   // the domain's gathers (indices into Program::gathers), ascending
    std::vector<std::int32_t> segments;  // the domain's segments (indices into Program::segments), ascending
    std::vector<std::int32_t> ordinal;   // Input domain: input ordinal per row; empty otherwise
    bool is_input = false;               // the group is one Input step
    bool is_const = false;               // the group is one Const step (nothing to reverse)
    bool is_scan = false;                // a scan domain: forward row by row, reverse rows backwards (D41)
    std::int32_t readers = 0;            // total "who reads me" entries over the domain's rows (all kinds)
  };
  std::vector<DomainPlan> domains;  // per Program domain
  int max_steps = 0;                // longest group
  std::size_t num_values = 0;

  std::size_t table_bytes() const noexcept;
};

// Derives the plan. Throws std::runtime_error if the program fails ir::validate,
// std::invalid_argument on a reserved op, a recurrent domain, a segment read by two steps, or
// an Input row without an input ordinal.
AdjointPlan build_plan(const ir::Program& program);

// The plan in readable form: per domain its rows, steps with their local rules and operand
// targets, edge slots and reader counts; then the totals.
std::string describe(const AdjointPlan& plan, const ir::Program& program);

}  // namespace epykos::adjoint
