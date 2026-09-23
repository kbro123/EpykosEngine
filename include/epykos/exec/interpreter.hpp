// EpykosEngine — the tiled vector interpreter over the domain IR (DESIGN.md §7 tier 2/3, D15).
//
// Executes an ir::Program for a batch of B states at once, with the same per-lane arithmetic as
// the scalar reference evaluator (ir/evaluate.hpp) and the tape replay (tape/replay.hpp): E0,
// bit-identical, whatever B, tile or lane tile is chosen.
//
// Layout (D15): one flat value buffer holding every domain's rows, batch axis innermost —
// value v of lane l lives at values[v·L + l], where L is the width of the lane chunk being
// evaluated. run(state, B, out) splits B into chunks of at most `lane_tile` lanes and, for each
// chunk, evaluates the whole program (domains in dependency order) for those lanes:
//
//   * an elementwise group runs one kernel at a time over a tile of `tile` rows × L lanes; every
//     intermediate is a contiguous vector of tile·L doubles in a preallocated scratch (L1-sized
//     at the default tile for small L), the last kernel writes straight into the value buffer.
//     Each kernel is picked at construction from (op, operand kinds, L): it loops over rows and
//     lanes with the operand sources (earlier step / literal / column / gather) resolved at
//     compile time, so there is no per-element dispatch. For L = 1 the row loop is the vector
//     axis; for L > 1 the lane loop is. Two consecutive steps that form a chain nobody else
//     reads — t = op(x, y) then op2(t, z) or op2(z, t), with op, op2 among add / sub / mul / div
//     / neg and x, y, z literals, columns or gathers — are one kernel (a fused pair): both
//     operations run in registers per lane, in the recorded order with the recorded roundings,
//     and t is never stored. A longer chain is a pair followed by single steps, except that an
//     Exp / Log of the pair's value that nothing else reads (a chain tail) is applied by the pair
//     kernel in place on each row it has just stored, while the row is in L1, so the libm
//     argument never round-trips through the scratch. On the M1 book the forwards, the exp
//     argument (with its exp as the tail), the float coupons and the swap PVs are pairs.
//     Options::fuse_pairs switches it off (one kernel per step);
//   * a Sum / Affine group is a whole-domain pass: rows are bucketed by segment length (a
//     structure-only permutation of independent rows) and the members of each bucket are stored
//     transposed, so fold step k is one vector op across the bucket's rows — each row's fold
//     stays the recorded left-to-right order (bit-identical), with no per-row dependency chain
//     stalling the pipeline and no variable trip count to mispredict;
//   * an elementwise domain whose rows are read only as members of such whole-domain Sum /
//     Affine groups (no gather, no output, no per-row Sum reads it) is never materialised:
//     each reduction block evaluates the producer's steps for the member rows it needs (the
//     same kernels, rows addressed through the block's member ids) and folds the result in
//     place — the per-element arithmetic and the fold order are unchanged (E0), the producer's
//     rows are never written to or read back from the value buffer. On the M1 book this is the
//     two coupon domains (31,806 rows) folded into the leg sums. Options::fuse_reductions
//     switches it off (every domain materialised);
//   * a domain read only through the gathers of one elementwise domain (no output, no segment
//     membership, no other reader; a whole-domain Sum / Affine only when every row has the same
//     materialised members, an interpolation) is not materialised either: each tile of the
//     consumer evaluates it for the rows the tile gathers into a tile-shaped temporary, which
//     the consumer's operand reads by tile row — the same kernels and fold order (E0), but no
//     write to and read back from a cold region of the value buffer. On the M1 book this is
//     the interpolated rates (2,561 rows) evaluated inside the exp domain's tiles.
//     Options::inline_producers switches it off;
//   * gathers and segment members read the value buffer of the current chunk.
//
// Runtime B and tile; kernels are instantiated for lane widths 1, 4, 8, 16, 32, 64 (a chunk of
// any other width runs the same source with a runtime lane loop), which keeps B a runtime value
// (D15) while giving the compiler a fixed lane count to vectorise. Zero allocations in run():
// the value buffer, scratch and plan tables are sized once in the constructor for max_batch.
//
// Thread safety: run() is const but uses the object's own scratch, so one Interpreter must not be
// run from two threads at once; construct one per thread (construction is cheap: it copies the
// program's index tables into its own layout).
//
// Exactness: ExpMode::std_exp evaluates Exp with std::exp, which is what the recorder, the
// replay and the reference evaluator use (E0). ExpMode::poly uses the vectorisable exp_poly of
// hand/exp_poly.hpp (≤ 1 ulp of std::exp: E1) for like-for-like timing against the hand-fused
// kernel's default; the E0 gates never use it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"

namespace epykos::exec {

enum class ExpMode : int { std_exp = 0, poly = 1 };

struct Options {
  int tile = 256;                    // rows per tile of an elementwise group / rows per segment bucket
  int max_batch = 64;                // lanes the buffers are sized for; run(B > max_batch) throws
  int lane_tile = 8;                 // lanes per chunk (B is split into chunks of at most this many)
  ExpMode exp = ExpMode::std_exp;    // std_exp: E0; poly: E1 (exp_poly), timing only
  bool fuse_reductions = true;       // evaluate reduction-only elementwise domains inside the reductions (E0); false: materialise every domain
  bool fuse_pairs = true;            // evaluate two consecutive chained steps in one kernel with the middle value in registers (E0); false: one kernel per step
  bool inline_producers = true;      // evaluate a domain read only through one elementwise domain's gathers inside that domain's tiles (E0); false: materialise it
};

class Interpreter {
 public:
  // Builds the plan for `program` (which must outlive the Interpreter). Throws
  // std::runtime_error if the program fails ir::validate, std::invalid_argument on a reserved
  // op, a recurrent domain, or bad options (tile < 1, max_batch < 1, lane_tile < 1).
  explicit Interpreter(const ir::Program& program, Options options = {});
  ~Interpreter();
  Interpreter(const Interpreter&) = delete;
  Interpreter& operator=(const Interpreter&) = delete;

  // state[k·B + b]: input ordinal k of lane b (n_inputs × B, SoA, batch innermost).
  // out[o·B + b]:  output ordinal o of lane b (n_outputs × B, SoA) — the tape's output order
  //                (on the M1 book: 1,000 swap PVs in swap order, then the book PV).
  // 1 <= B <= max_batch(). No allocation.
  void run(const double* state, int B, double* out) const;

  // The plan: options, domain order with rows / steps / kernel per step / tiles, segment
  // buckets, fused producers, and buffer sizes. For the benchmark report.
  std::string describe() const;

  const Options& options() const noexcept;
  const ir::Program& program() const noexcept;
  int n_inputs() const noexcept;
  int n_outputs() const noexcept;
  int max_batch() const noexcept;
  std::size_t num_values() const noexcept;   // rows over all domains (one value per row per lane)
  std::size_t num_fused_values() const noexcept;  // of which rows of fused domains (never materialised)
  std::size_t value_bytes() const noexcept;  // the value buffer, as allocated
  std::size_t scratch_bytes() const noexcept;  // step scratch + operand temporaries + reduction accumulator and member buffers
  std::size_t table_bytes() const noexcept;    // the plan's own index / coefficient tables

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char* to_string(ExpMode mode) noexcept;

}  // namespace epykos::exec
