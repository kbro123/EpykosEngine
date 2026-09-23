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
// stores every row value at values[v·L + l] (L = lanes in the chunk, D29: row values kept,
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
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "epykos/adjoint/plan.hpp"
#include "epykos/ir/program.hpp"

namespace epykos::adjoint {

struct Options {
  int tile = 256;      // rows per tile of a group's forward / reverse pass
  int max_batch = 64;  // lanes the buffers are sized for; run(B > max_batch) throws
  int lane_tile = 8;   // lanes per chunk (B is split into chunks of at most this many)
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
  const ir::Program& program() const noexcept;
  int n_inputs() const noexcept;
  int n_outputs() const noexcept;
  int max_batch() const noexcept;
  std::size_t num_values() const noexcept;    // rows over all domains
  std::size_t value_bytes() const noexcept;   // the value buffer and the value-adjoint buffer, as allocated
  std::size_t edge_bytes() const noexcept;    // the gather and segment edge-slot adjoint buffers
  std::size_t scratch_bytes() const noexcept; // per-tile step values, operand loads and step adjoints
  std::size_t table_bytes() const noexcept;   // the plan's tables

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace epykos::adjoint
