// EpykosEngine — private plan structures of the tiled interpreter (src/exec only).
//
// The plan is the Program re-expressed for execution: per step a kernel per lane-width variant
// and operand descriptors with their sources resolved to pointers; per Sum / Affine group the
// length-bucketed, transposed segment tables. Everything is built once in the constructor and
// read-only during run().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/tape/op.hpp"

namespace epykos::exec::detail {

// Where a kernel reads an operand from, resolved at construction.
enum class Kind : std::uint8_t {
  Vec = 0,  // a tile-shaped vector: an earlier step's scratch or an operand temporary, [i·L + l]
  Lit = 1,  // one double, broadcast
  Col = 2,  // one double per row of the domain, broadcast across lanes
  Gat = 3,  // a value id per row: read from the value buffer, [id·L + l]
};

const char* to_string(Kind k) noexcept;

struct Operand {
  Kind kind = Kind::Lit;
  const double* data = nullptr;        // Vec: the buffer; Lit: &value; Col: values by row
  const std::int32_t* index = nullptr; // Gat: value id by row
};

// Lane-width variants the kernels are instantiated for. Variant 0 is the runtime-L fallback.
inline constexpr int lane_variants[] = {0, 1, 4, 8, 16, 32, 64};
inline constexpr int n_lane_variants = 7;
inline int lane_variant(int L) noexcept {
  for (int v = 1; v < n_lane_variants; ++v) {
    if (lane_variants[v] == L) return v;
  }
  return 0;
}

// Per-run context handed to every kernel.
struct RunCtx {
  double* values = nullptr;       // the value buffer of the current chunk, stride L
  const double* state = nullptr;  // inputs, state[k·B + b]
  int B = 0;                      // batch width of the run
  int b0 = 0;                     // first lane of the chunk
  int L = 0;                      // lanes in the chunk
  double* acc = nullptr;          // segment accumulator, tile·Lmax doubles
};

// A bucket of rows of one segment length, stored transposed: memT[k·n + i] is member k of the
// bucket's row i; coefT likewise for Affine.
struct SegBlock {
  std::int32_t n = 0;       // rows
  std::int32_t len = 0;     // members per row
  std::size_t row0 = 0;     // offset into SegPlan::rows
  std::size_t memT = 0;     // offset into SegPlan::memT / coefT (len·n entries)
};

struct SegPlan {
  bool affine = false;
  Operand konst;                       // Affine c_0 (Lit or Col)
  std::vector<std::int32_t> rows;      // destination row per bucket entry
  std::vector<std::int32_t> memT;      // transposed member value ids
  std::vector<double> coefT;           // transposed coefficients (Affine only)
  std::vector<SegBlock> blocks;
  // The untransposed tables, for a Sum / Affine step that is not its group's only step.
  const std::int32_t* offsets = nullptr;
  const std::int32_t* members = nullptr;
  const double* coefs = nullptr;
  std::int32_t min_len = 0, max_len = 0;
};

struct StepPlan;
using OpKernel = void (*)(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* out);
using LoadKernel = void (*)(const Operand& src, const RunCtx& ctx, int r0, int n, double* out);
using SegKernel = void (*)(const SegPlan& sp, const RunCtx& ctx, double* domain_values);

struct PreLoad {
  Operand src;
  double* dst = nullptr;
  LoadKernel fn[n_lane_variants] = {};
};

struct StepPlan {
  Op op = Op::Const;
  Operand a, b, c, konst;
  int n_pre = 0;
  PreLoad pre[3];                        // operand materialisation for Fma / Select
  OpKernel fn[n_lane_variants] = {};
  const SegPlan* seg = nullptr;          // Sum / Affine (per-tile fallback)
  const std::int32_t* ordinal = nullptr; // Input: input ordinal by row
  double* out = nullptr;                 // scratch for a non-final step; nullptr = the value buffer
  std::string name;                      // for describe()
};

struct GroupPlan {
  std::int32_t domain = -1;
  std::int32_t rows = 0;
  std::int32_t value_base = 0;
  std::vector<StepPlan> steps;
  // A group that is one Sum / Affine step runs as a whole-domain pass.
  bool whole_segment = false;
  SegPlan seg;
  SegKernel seg_fn[n_lane_variants] = {};
  std::vector<std::int32_t> ordinal;     // Input domain: ordinal by row
};

// The kernel table for one lane width (kernels.hpp / kernels_impl.hpp).
template <int L>
struct KernelTable {
  static OpKernel binary(Op op, Kind ka, Kind kb);      // Add Sub Mul Div CmpLt..CmpEq
  static OpKernel unary(Op op, Kind ka);                // Neg Exp Log Sqrt Recip
  static OpKernel exp_poly(Kind ka);                    // Exp via exp_poly (E1)
  static OpKernel ternary(Op op);                       // Fma Select over three Vec operands
  static OpKernel konst(Kind kk);                       // Const: broadcast konst (Lit / Col)
  static OpKernel input();                              // Input: copy from the state
  static OpKernel seg_rows(bool affine);                // Sum / Affine per tile (fallback)
  static LoadKernel load(Kind k);                       // materialise an operand
  static SegKernel seg_whole(bool affine);              // Sum / Affine whole-domain, bucketed
};

extern template struct KernelTable<0>;
extern template struct KernelTable<1>;
extern template struct KernelTable<4>;
extern template struct KernelTable<8>;
extern template struct KernelTable<16>;
extern template struct KernelTable<32>;
extern template struct KernelTable<64>;

}  // namespace epykos::exec::detail
