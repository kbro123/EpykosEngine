// EpykosEngine — the hand-fused reference kernel for the M1 book (M1/P5).
//
// What the generic path (signature pass → domain IR → interpreter/catalogue) has to match on the
// M1 book, written as a performance engineer would from docs/DESIGN.md §5–§7 and the expected
// domain chain of docs/WORKLOADS.md §M1:
//
//   Knots —linmap→ Times —exp→ DF —gather→ Coupons —segment_sum→ Legs —segment_sum→ Swaps → Book
//
// Tables are built once in the constructor from the Book (structure only, D16):
//   * the unique discount times of the book (every coupon end, plus every non-realised float
//     coupon start), each with its two knot indices and linear weights: the block-sparse W of
//     the linmap, stored as (knot0, knot1, w0, w1) rows (R7: two non-zeros per row). Times are
//     numbered in order of first use along the processing order, so a seasoned swap's private
//     times are consecutive rows of the DF table and its coupon rows stream through memory
//     sequentially (measured: 2.8× on those rows against time-sorted numbering);
//   * coupon rows as gather indices into the DF array with the constant columns folded (R1):
//     "plain" rows pv = coef·DF(e) with coef = N·τ·K (fixed) or N·τ·R (the realised first float
//     coupon of a seasoned swap), and "float" rows pv = N·τ·fwd·DF(e), fwd = (DF(s)/DF(e) − 1)/τ,
//     bucketed by kind (R2) so no row branches on its kind;
//   * segment offsets coupon → leg → swap in CSR form, and the side sign per swap;
//   * swaps grouped by (tenor, seasoned), a structure-only permutation: within a group every loop
//     has the same trip count, so the branch predictor is never surprised and consecutive swaps'
//     accumulation chains overlap in the out-of-order window. Results are scattered back to swap
//     order; the fold order within each leg and over the book is untouched.
//
// Per eval (no allocation; scratch is owned by the kernel):
//   1. z(t) at the unique times through W (a 2-term GEMV per time);
//   2. DF = exp(−z(t)·t) once per unique time (R4a), plus 1/DF(e) once per time when the shared
//      reciprocal is on (R4b);
//   3. one fused pass over the coupon rows: gather DF(s), DF(e) [and 1/DF(e)], row pv with fma,
//      folded left to right into the leg accumulator (R5: gathers in, segment-sum epilogue out);
//   4. swap pv = side·(fixed − float); 5. book pv = left fold over swaps in swap order.
// The batch version puts the batch axis innermost on every table (D15): DF is [times × B];
// the coupon pass runs lane tiles of 32 (eight AVX2 vectors per leg accumulator) with the tile
// loop outside the swaps of a group, keeping a group's DF lines in L1 across its swaps; every
// inner loop is plain C++ the compiler auto-vectorises (no intrinsics). The batch path always
// computes max_batch lanes (B <= max_batch of them are written), so its row offsets into the DF
// table are constructor-time constants.
//
// Arithmetic and exactness (D8):
//   HandArith::fused      fma accumulation, ×(1/τ) instead of ÷τ, optionally DF(s)·(1/DF(e)) with
//                         one reciprocal per time, exp_poly: E1 against the templated double maths
//                         (rounding-level; gated at 1e-12 relative to the leg scale).
//   HandArith::reference  the oracle's own operation order (÷, no fma, std::exp): E0 bitwise
//                         against price_book<double> compiled with -ffp-contract=off. This is the
//                         structural check that the tables are exactly right; it is not the fast
//                         kernel. It forces shared_reciprocal = false and exp = std_exp.
// Its source is an E0 TU (bench/hand/m1_hand_kernel_e0.cpp, pinned by bench/hand/CMakeLists.txt to
// -ffp-contract=off in every preset on every compiler, like the book generator; D25, D28), so its
// bits do not depend on the compiler's contraction choices: the only fused operations are the
// explicit std::fma calls of the fused mode.

//
// eval_batch(B) is bitwise equal to B calls of eval on the corresponding lanes: every lane runs
// the same IEEE operations in the same order; vectorisation never reassociates.
//
// Thread safety: const methods use the kernel's own scratch, so one kernel object must not be
// evaluated concurrently from two threads; construct one per thread (construction is cheap).
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"

namespace epykos::hand {

enum class HandExp : int { poly = 0, std_exp = 1 };
enum class HandArith : int { fused = 0, reference = 1 };

struct M1HandOptions {
  HandArith arith = HandArith::fused;
  bool shared_reciprocal = true;  // fused mode only: one 1/DF(e) per unique time, shared by the float rows that read it
  HandExp exp = HandExp::poly;    // reference mode forces std_exp
  int max_batch = fixtures::n_states;  // scratch is sized once for this many lanes; eval_batch(B > max_batch) throws
};

class M1HandKernel {
 public:
  explicit M1HandKernel(const fixtures::Book& book, M1HandOptions options = {});

  // z[0..12): swap_pv[0..1000) in swap order, *book_pv = left fold of swap_pv.
  void eval(const double* z, double* swap_pv, double* book_pv) const;

  // SoA, batch innermost: z[k·B + b], swap_pv[i·B + b], book_pv[b]. 1 <= B <= max_batch().
  void eval_batch(const double* z, int B, double* swap_pv, double* book_pv) const;

  // Structure, for tests and reports.
  const M1HandOptions& options() const noexcept { return opt_; }
  int n_swaps() const noexcept { return n_swaps_; }
  int n_knots() const noexcept { return n_knots_; }
  int n_times() const noexcept { return n_times_; }          // unique discount times
  int n_plain_rows() const noexcept { return n_plain_; }     // fixed coupons + realised first coupons
  int n_float_rows() const noexcept { return n_float_; }     // float coupons with a forward
  int max_batch() const noexcept { return opt_.max_batch; }
  int lane_tile() const noexcept;                            // the batch lane tile (32)
  int n_groups() const noexcept { return n_groups_; }        // (tenor, seasoned) groups
  const std::vector<double>& times() const noexcept { return time_; }        // first-use order
  const std::vector<int>& swap_order() const noexcept { return swap_of_; }  // position -> swap

 private:
  template <int L>
  void eval_impl(const double* z, int B, double* swap_pv, double* book_pv) const;
  template <int L>
  void curve_pass() const;  // stride 1 (L = 1) or max_stride_
  // L = 1: stride 1, row indices are time indices; L > 1: stride max_stride_, row offsets are
  // premultiplied (time × max_stride_).
  template <int L, bool Shared, bool Ref>
  void coupon_pass(int B, double* swap_pv) const;

  M1HandOptions opt_;
  int n_swaps_ = 0;
  int n_knots_ = 0;

  // Times and the block-sparse W: z(t_u) = w0[u]·z[knot0[u]] + w1[u]·z[knot1[u]].
  int n_times_ = 0;
  std::vector<double> time_;
  std::vector<int> knot0_, knot1_;
  std::vector<double> w0_, w1_;

  // Swaps in processing order: position pos holds swap swap_of_[pos]. Groups are runs of
  // positions [group_begin_[g], group_begin_[g+1]) sharing group_tenor_[g] and group_seasoned_[g].
  int n_groups_ = 0;
  std::vector<int> swap_of_;
  std::vector<int> group_begin_, group_tenor_, group_seasoned_;

  // Plain rows of position pos: [plain_begin[pos], plain_begin[pos] + T) its fixed leg in period
  // order, then its realised first float coupon (0 or 1 row).
  int n_plain_ = 0;
  std::vector<int> plain_begin_;
  std::vector<int> plain_time_;      // index of e_j in the time table
  std::vector<int> plain_off_;       // plain_time_ × max_stride_ (batch path)
  std::vector<double> plain_coef_;   // N·τ·K or N·τ·R

  // Float rows of position pos: [float_begin[pos], float_begin[pos] + T − seasoned) in period order.
  int n_float_ = 0;
  std::vector<int> float_begin_;
  std::vector<int> float_s_, float_e_;  // indices of s_j and e_j in the time table
  std::vector<int> float_s_off_, float_e_off_;  // × max_stride_ (batch path)
  std::vector<double> float_coef_;      // N·τ
  std::vector<double> float_tau_;       // 1/τ (fused) or τ (reference)

  std::vector<double> side_;  // ±1.0, by position

  // Scratch (batch innermost, stride = B rounded up to the lane tile), sized in the constructor.
  int max_stride_ = 0;
  mutable std::vector<double> zp_;   // [knots × stride]
  mutable std::vector<double> df_;   // [times × stride]
  mutable std::vector<double> rcp_;  // [times × stride], shared reciprocal only
  mutable std::vector<double> leg_;  // [max group × lane tile], a group's fixed legs for one tile
  mutable std::vector<double> acc_;  // [stride], book fold
  int max_group_ = 0;
};

}  // namespace epykos::hand
