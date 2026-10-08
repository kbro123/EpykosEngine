// EpykosEngine — the mechanical adjoint over the domain IR (DESIGN.md §7 "Adjoints"; M2/Q3).
//
// Adjoint::run evaluates a Program forward for a batch of B states (the same per-lane
// arithmetic as exec::Interpreter and ir::Evaluator: E0, bit-identical) and then, for a seed
// out_bar over the outputs, the reverse pass of plan.hpp: state_bar = Jᵀ · out_bar with
// J = ∂outputs/∂state. Seeding out_bar = e_o gives the gradient of output o (the book PV, one
// swap PV); seeding a lane per swap gives per-swap gradients in one batched call.
//
// Layout (D15, shared with exec::Interpreter): batch axis innermost everywhere. state[k·B + b]
// and state_bar[k·B + b] per input ordinal k; out[o·B + b] and out_bar[o·B + b] per output
// ordinal o. B is split into chunks of at most `lane_tile` lanes; for each chunk the forward pass
// stores every row value at values[v·L + l] (L = lanes in the chunk, D31: row values kept,
// intermediate steps recomputed per tile in the reverse) and writes the outputs, then the
// reverse walks the domains backwards: pull the domain's v̄ from the edge slots of its readers
// (plan.hpp), then reverse the group in tiles of `tile` rows — recompute the intermediate steps,
// run the local rules from the last step down, accumulating into the earlier steps' adjoints and
// into the (gather, row) / (segment, row) edge slots. Every operation is per (row, lane) with no
// cross-lane or cross-row reduction and no dependence on the tile boundaries, so a lane of a
// batched run is bitwise the B = 1 run of that state and every tile / lane_tile gives the same
// bits (the E0 tests assert both). The kernels are compiled with -ffp-contract=off in every
// preset (src/adjoint/*_e0.cpp, D25).
//
// Accumulation orders (fixed, documented, deterministic; they need not match any other
// implementation): within a group, steps are reversed from the last to the first and an op's
// operands contribute in the order a, b, c, each `+=` into the target's running sum; a value's
// pull sums its readers in the order of plan.hpp. Zero allocations in run(): every buffer is
// sized in the constructor for min(lane_tile, max_batch) lanes.
//
// Thread safety: run() is const but uses the object's own buffers; one Adjoint per thread.
//
// Two programs, and they are not the same program (PRINCIPLES.md §1b, invariant I1; M5/C4).
// `program()` returns the FORWARD program this object was built from — an input. `to_program()`
// returns the REVERSE of it as an `ir::Program` — the derivative, bitwise what `run` computes.
// Read both declarations below before using either; §1b names `program()`'s meaning as I1's
// symptom and it is deliberately unchanged.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "epykos/adjoint/plan.hpp"
#include "epykos/catalogue/coverage.hpp"
#include "epykos/ir/program.hpp"

namespace epykos::adjoint {

struct Options {
  int tile = 256;      // rows per tile of a group's forward / reverse pass
  int max_batch = 64;  // lanes the buffers are sized for; run(B > max_batch) throws
  int lane_tile = 8;   // lanes per chunk (B is split into chunks of at most this many)
  // Run a chunk group whose lane count has no kernel of its own as a sequence of specialised
  // widths (2 -> 1+1, 5 -> 4+1, 7 -> 4+1+1+1) instead of as one generic runtime-L chunk — see
  // exec/lanes.hpp, which holds the rule, and `exec::Options::split_lane_chunks`, which is the
  // same switch on the forward runtime. true by default; false is the pre-D103 chunking.
  bool split_lane_chunks = true;
  // M4/C1 (PROBLEM.md §7, DESIGN.md §7 "As built (M4/R0...)": "the reverse of a fused group is
  // rewrite / catalogue work (M4)"): the forward pass below materialises every domain's rows
  // unconditionally (unlike exec::Interpreter, Adjoint applies none of the fuse/inline
  // optimisations of its own), so a catalogued kernel is always a safe drop-in replacement for
  // ANY catalogue-eligible domain's forward materialisation, whatever this Program's structure —
  // see adjoint_e0.cpp's forward(). true by default; false disables the lookup entirely (the
  // differential gate's "registry off" side).
  bool use_catalogue = true;
};

class Adjoint {
 public:
  // Builds the plan for `program` (which must outlive the Adjoint) and allocates the buffers.
  // Throws std::runtime_error if the program fails ir::validate, std::invalid_argument on a
  // reserved op, a recurrent domain, or bad options (tile < 1, max_batch < 1, lane_tile < 1).
  explicit Adjoint(const ir::Program& program, Options options = {});
  ~Adjoint();
  Adjoint(const Adjoint&) = delete;
  Adjoint& operator=(const Adjoint&) = delete;

  // state[k·B + b]      input ordinal k of lane b (n_inputs × B, SoA)
  // out_bar[o·B + b]    seed: the adjoint of output ordinal o in lane b (n_outputs × B, SoA)
  // out[o·B + b]        the forward outputs (n_outputs × B, SoA); may be null (not written)
  // state_bar[k·B + b]  the result: Σ_o out_bar[o, b] · ∂output_o/∂state_k for lane b
  // 1 <= B <= max_batch(). No allocation. Throws std::invalid_argument on a bad B.
  void run(const double* state, int B, const double* out_bar, double* out, double* state_bar) const;

  // The plan (plan.hpp describe) with the options and buffer sizes.
  std::string describe() const;

  const AdjointPlan& plan() const noexcept;
  const Options& options() const noexcept;

  // THE FORWARD PROGRAM this Adjoint was constructed from. It is an INPUT, not a result: it is
  // the P of `state -> outputs`, and nothing about the derivative is in it. PRINCIPLES.md §1b
  // names exactly this as invariant I1's symptom -- "`Adjoint` is constructed from the FORWARD
  // program and `program()` returns that same forward program". The meaning of this method has
  // not changed and must not: `to_program()` below is the derivative.
  const ir::Program& program() const noexcept;

  // THE REVERSE OF `program()`, AS A PROGRAM -- I1's `adjoint(P)`, and the public face of the
  // capability (M5/C4). Not the same object as `program()` and not a view of it: a new
  // `ir::Program` Q with
  //
  //   inputs   P's inputs in P's input order, then one per OUTPUT ordinal of P: the out_bar seed
  //   outputs  P's outputs in P's output order, then one per INPUT ordinal of P: the state adjoint
  //
  // so Q takes (state, out_bar) flat and returns (out, state_bar) flat, and running Q through
  // `ir::Evaluator` or `exec::Interpreter` reproduces `run(state, 1, out_bar, out, state_bar)`
  // BITWISE -- §1b's gate, asserted by tests/adjoint/i1_gate_e0_test.cpp over every fixture the
  // adjoint is gated on, Stage A included. Batch: Q's own layout is Interpreter's, batch
  // innermost, so a B-lane Interpreter run of Q is the B-lane `run` above lane for lane.
  //
  // Derived from the plan THIS Adjoint already built, so it cannot disagree with what `run`
  // does; `adjoint_to_program(P)` in adjoint/adjoint_to_program.hpp is the same function for a
  // caller that has no Adjoint. O(program) work and a fresh allocation on every call -- hold the
  // result, do not call it in a loop. Throws what `adjoint_to_program` throws (std::logic_error
  // on a recurrence whose reverse would carry two values per row).
  ir::Program to_program() const;


  int n_inputs() const noexcept;
  int n_outputs() const noexcept;
  int max_batch() const noexcept;
  std::size_t num_values() const noexcept;    // rows over all domains
  std::size_t value_bytes() const noexcept;   // the value buffer and the value-adjoint buffer, as allocated
  std::size_t edge_bytes() const noexcept;    // the gather and segment edge-slot adjoint buffers
  std::size_t scratch_bytes() const noexcept; // per-tile step values, operand loads and step adjoints
  std::size_t table_bytes() const noexcept;   // the plan's tables

  // M4/C1: how much of this Adjoint's own forward materialisation the catalogue serves — by
  // domain count, by row count, and (built with -DEPYKOS_EXEC_PROFILE, after >= 1 run()) by this
  // instance's own measured wall-clock time. See include/epykos/catalogue/coverage.hpp.
  catalogue::Coverage coverage() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace epykos::adjoint
