// EpykosEngine — the interpreter's kernels, templated on op, operand kinds, row mode and lane
// width L (L = 0: runtime lane count from the context). Included by one TU per lane width
// (kernels_l*.cpp), which explicitly instantiates KernelTable<L>.
//
// Every kernel loops rows (i over the tile; the domain row r is r0 + i, or idx[i] + r0 in the
// indirect row mode, see plan.hpp) and lanes (l < L) and performs exactly the IEEE operation the
// scalar evaluator performs, in the same order; the only freedom taken is which loop the compiler
// vectorises. Contraction is off in the including TUs (see kernels_l*.cpp), so an Affine fold's
// product and sum round separately.
//
// How the loops are written, and why. A tile's output never overlaps what the tile reads (rows
// of a non-recurrent domain read other domains, earlier steps' scratch, columns and literals),
// but the compiler cannot see that through pointers it loads from the plan, and __restrict on an
// inlined body's parameters is not carried through inlining by the toolchain in use (measured:
// no scoped no-alias metadata, the lane loop stayed scalar). So every kernel stages one row's
// lanes (L > 1) or one block of rows (L = 1) in small local arrays — identified objects the
// compiler knows nothing else aliases — computes on those, and copies out. With L a compile-time
// constant this becomes a broadcast, L/4 packed operations and L/4 packed stores per row; with
// L = 1 the block of rows is the vector axis (gathers included). Sum / Affine keep several rows'
// accumulators in locals so that several fold chains are in flight at once. The runtime-L
// variant (L = 0, chunks of a width no specialised kernel exists for) keeps plain loops.
//
// Reductions over fused producers (plan.hpp: GroupPlan::fused). A block whose member positions
// come from a fused producer evaluates that producer's group for the member rows (indirect row
// mode, idx = the transposed member ids, one call per run of positions with the same producer):
// the earlier steps into their scratch, the last step through a reduction epilogue that folds
// each value into its row's accumulator with several rows in flight in locals (Sum: acc = m_0,
// acc = acc + m_k; Affine: p = c_k·m_k, acc = acc + p — the recorded order). Members of a
// materialised domain are folded straight from the value buffer. The per-element arithmetic
// and the fold order are those of the materialising path, so the result is bit-identical; what
// changes is that the producer's rows are never written to or read back from the value buffer.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

#include "epykos/hand/exp_poly.hpp"
#include "plan.hpp"

namespace epykos::exec::detail {

// The per-row helpers (row groups of a reduction, the fold epilogues) must be inlined into the
// kernel that calls them: as separate functions they take a dozen arguments through the stack
// per row, which at 32 lanes per row costs as much as the row's arithmetic (measured).
#define EPYKOS_EXEC_INLINE inline __attribute__((always_inline))

// Rows per block when L = 1 (the row axis is the vector axis).
inline constexpr int row_block = 16;
// Rows whose accumulators a Sum / Affine keeps in flight at once for lane width L (32 lanes,
// i.e. eight 4-wide accumulators, at every L; one row at L >= 32).
template <int L>
inline constexpr int seg_rows_in_flight = L >= 32 ? 1 : (L == 1 ? row_block : 32 / L);

template <int L>
inline int lanes(const RunCtx& ctx) noexcept {
  if constexpr (L > 0) {
    (void)ctx;
    return L;
  } else {
    return ctx.L;
  }
}

constexpr bool is_scalar_kind(Kind k) noexcept { return k == Kind::Lit || k == Kind::Col; }

// The domain row of tile row i (plan.hpp: row modes).
template <bool Ind>
inline int row_at(int r0, const std::int32_t* idx, int i) noexcept {
  if constexpr (Ind) {
    return idx[i] + r0;
  } else {
    (void)idx;
    return r0 + i;
  }
}

// ---- operand access ------------------------------------------------------------------------

// One element (L = 1 or the runtime variant's lane l): row r / tile row i / lane l of stride LL.
// A gathered operand reads its own base (Operand::data: the value buffer, or an inlined
// producer's tile temporary), so `values` is unused for it.
template <Kind K>
inline double elem(const double* values, const Operand& o, int r, std::size_t i, int l, std::size_t LL) noexcept {
  (void)values;
  if constexpr (K == Kind::Vec) {
    (void)r;
    return o.data[i * LL + static_cast<std::size_t>(l)];
  } else if constexpr (K == Kind::Lit) {
    (void)r; (void)i; (void)l; (void)LL;
    return o.data[0];
  } else if constexpr (K == Kind::Col) {
    (void)i; (void)l; (void)LL;
    return o.data[r];
  } else {
    (void)i;
    return o.data[static_cast<std::size_t>(o.index[r]) * LL + static_cast<std::size_t>(l)];
  }
}

// L > 1: stage row r's lanes of a vector-kind operand in `dst`; return the scalar of a scalar kind.
template <Kind K, int L>
inline double stage_row(double* dst, const double* values, const Operand& o, int r, std::size_t i) noexcept {
  (void)values;
  if constexpr (K == Kind::Vec) {
    (void)r;
    const double* src = o.data + i * static_cast<std::size_t>(L);
    for (int l = 0; l < L; ++l) dst[l] = src[l];
    return 0.0;
  } else if constexpr (K == Kind::Gat) {
    (void)i;
    const double* src = o.data + static_cast<std::size_t>(o.index[r]) * static_cast<std::size_t>(L);
    for (int l = 0; l < L; ++l) dst[l] = src[l];
    return 0.0;
  } else {
    (void)dst; (void)values; (void)i;
    return K == Kind::Lit ? o.data[0] : o.data[r];
  }
}

template <Kind K>
inline double lane(double s, const double* v, int l) noexcept {
  if constexpr (is_scalar_kind(K)) {
    (void)v; (void)l;
    return s;
  } else {
    (void)s;
    return v[l];
  }
}

// L = 1: stage tile rows [i, i + nb) of an operand in dst[0..nb).
template <Kind K, bool Ind>
inline void stage_rows1(double* dst, int nb, const double* values, const Operand& o, int r0, const std::int32_t* idx,
                        int i) noexcept {
  for (int j = 0; j < nb; ++j) {
    dst[j] = elem<K>(values, o, row_at<Ind>(r0, idx, i + j), static_cast<std::size_t>(i + j), 0, 1);
  }
}

template <Op op>
inline double apply2(double a, double b) noexcept {
  if constexpr (op == Op::Add) return a + b;
  else if constexpr (op == Op::Sub) return a - b;
  else if constexpr (op == Op::Mul) return a * b;
  else if constexpr (op == Op::Div) return a / b;
  else if constexpr (op == Op::CmpLt) return (a < b) ? 1.0 : 0.0;
  else if constexpr (op == Op::CmpLe) return (a <= b) ? 1.0 : 0.0;
  else if constexpr (op == Op::CmpGt) return (a > b) ? 1.0 : 0.0;
  else if constexpr (op == Op::CmpGe) return (a >= b) ? 1.0 : 0.0;
  else if constexpr (op == Op::CmpEq) return (a == b) ? 1.0 : 0.0;
  else static_assert(op == Op::Add, "not a binary op");
}

template <Op op>
inline double apply1(double a) noexcept {
  if constexpr (op == Op::Neg) return -a;
  else if constexpr (op == Op::Exp) return std::exp(a);
  else if constexpr (op == Op::Log) return std::log(a);
  else if constexpr (op == Op::Sqrt) return std::sqrt(a);
  else if constexpr (op == Op::Recip) return 1.0 / a;
  else static_assert(op == Op::Neg, "not a unary op");
}

template <Op op>
inline double apply3(double a, double b, double c) noexcept {
  if constexpr (op == Op::Fma) return std::fma(a, b, c);
  else if constexpr (op == Op::Select) return (a != 0.0) ? b : c;
  else static_assert(op == Op::Fma, "not a ternary op");
}

// ---- elementwise -------------------------------------------------------------------------

template <Op op, Kind KA, Kind KB, int L, bool Ind>
void k_binary(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  const double* values = ctx.values;
  const Operand& a = s.a;
  const Operand& b = s.b;
  if constexpr (L == 1) {
    int i = 0;
    for (; i + row_block <= n; i += row_block) {
      double va[row_block], vb[row_block], o[row_block];
      stage_rows1<KA, Ind>(va, row_block, values, a, r0, idx, i);
      stage_rows1<KB, Ind>(vb, row_block, values, b, r0, idx, i);
      for (int j = 0; j < row_block; ++j) o[j] = apply2<op>(va[j], vb[j]);
      for (int j = 0; j < row_block; ++j) out[i + j] = o[j];
    }
    for (; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      out[i] = apply2<op>(elem<KA>(values, a, r, static_cast<std::size_t>(i), 0, 1),
                          elem<KB>(values, b, r, static_cast<std::size_t>(i), 0, 1));
    }
  } else if constexpr (L > 1) {
    for (int i = 0; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      double va[L], vb[L], o[L];
      const double sa = stage_row<KA, L>(va, values, a, r, static_cast<std::size_t>(i));
      const double sb = stage_row<KB, L>(vb, values, b, r, static_cast<std::size_t>(i));
      for (int l = 0; l < L; ++l) o[l] = apply2<op>(lane<KA>(sa, va, l), lane<KB>(sb, vb, l));
      double* dst = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) dst[l] = o[l];
    }
  } else {
    const std::size_t LL = static_cast<std::size_t>(ctx.L);
    for (int i = 0; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      double* dst = out + static_cast<std::size_t>(i) * LL;
      for (int l = 0; l < ctx.L; ++l) {
        dst[l] = apply2<op>(elem<KA>(values, a, r, static_cast<std::size_t>(i), l, LL),
                            elem<KB>(values, b, r, static_cast<std::size_t>(i), l, LL));
      }
    }
  }
}

template <Op op, Kind KA, int L, bool Ind>
void k_unary(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  const double* values = ctx.values;
  const Operand& a = s.a;
  if constexpr (KA == Kind::Vec && (op == Op::Exp || op == Op::Log)) {
    // A libm call per element over a contiguous tile: nothing to vectorise, no staging.
    (void)values; (void)r0; (void)idx;
    const double* src = a.data;
    const std::size_t total = static_cast<std::size_t>(n) * static_cast<std::size_t>(lanes<L>(ctx));
    for (std::size_t e = 0; e < total; ++e) out[e] = apply1<op>(src[e]);
  } else if constexpr (L == 1) {
    int i = 0;
    for (; i + row_block <= n; i += row_block) {
      double va[row_block], o[row_block];
      stage_rows1<KA, Ind>(va, row_block, values, a, r0, idx, i);
      for (int j = 0; j < row_block; ++j) o[j] = apply1<op>(va[j]);
      for (int j = 0; j < row_block; ++j) out[i + j] = o[j];
    }
    for (; i < n; ++i) {
      out[i] = apply1<op>(elem<KA>(values, a, row_at<Ind>(r0, idx, i), static_cast<std::size_t>(i), 0, 1));
    }
  } else if constexpr (L > 1) {
    for (int i = 0; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      double va[L], o[L];
      const double sa = stage_row<KA, L>(va, values, a, r, static_cast<std::size_t>(i));
      for (int l = 0; l < L; ++l) o[l] = apply1<op>(lane<KA>(sa, va, l));
      double* dst = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) dst[l] = o[l];
    }
  } else {
    const std::size_t LL = static_cast<std::size_t>(ctx.L);
    for (int i = 0; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      double* dst = out + static_cast<std::size_t>(i) * LL;
      for (int l = 0; l < ctx.L; ++l) dst[l] = apply1<op>(elem<KA>(values, a, r, static_cast<std::size_t>(i), l, LL));
    }
  }
}

// Materialise an operand into a tile-shaped vector.
template <Kind K, int L, bool Ind>
void load_impl(const Operand& a, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  const double* values = ctx.values;
  if constexpr (L == 1) {
    int i = 0;
    for (; i + row_block <= n; i += row_block) {
      double va[row_block];
      stage_rows1<K, Ind>(va, row_block, values, a, r0, idx, i);
      for (int j = 0; j < row_block; ++j) out[i + j] = va[j];
    }
    for (; i < n; ++i) out[i] = elem<K>(values, a, row_at<Ind>(r0, idx, i), static_cast<std::size_t>(i), 0, 1);
  } else if constexpr (L > 1) {
    for (int i = 0; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      double va[L];
      const double sa = stage_row<K, L>(va, values, a, r, static_cast<std::size_t>(i));
      double* dst = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) dst[l] = lane<K>(sa, va, l);
    }
  } else {
    const std::size_t LL = static_cast<std::size_t>(ctx.L);
    for (int i = 0; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      double* dst = out + static_cast<std::size_t>(i) * LL;
      for (int l = 0; l < ctx.L; ++l) dst[l] = elem<K>(values, a, r, static_cast<std::size_t>(i), l, LL);
    }
  }
}

template <Kind K, int L, bool Ind>
void k_load(const Operand& a, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  load_impl<K, L, Ind>(a, ctx, r0, idx, n, out);
}

// Exp through exp_poly: materialise the argument, then transform in place (E1).
template <Kind KA, int L, bool Ind>
void k_exp_poly(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  load_impl<KA, L, Ind>(s.a, ctx, r0, idx, n, out);
  hand::exp_poly_array(out, out, n * lanes<L>(ctx));
}

// Fma / Select over three materialised (Vec) operands: three scratch buffers, the output is a
// fourth; staged through locals so the loop vectorises without alias checks. Row-mode agnostic.
template <Op op, int L>
void k_ternary(const StepPlan& s, const RunCtx& ctx, int, const std::int32_t*, int n, double* out) {
  const double* a = s.a.data;
  const double* b = s.b.data;
  const double* c = s.c.data;
  const std::size_t total = static_cast<std::size_t>(n) * static_cast<std::size_t>(lanes<L>(ctx));
  std::size_t e = 0;
  for (; e + row_block <= total; e += row_block) {
    double va[row_block], vb[row_block], vc[row_block], o[row_block];
    for (int j = 0; j < row_block; ++j) va[j] = a[e + static_cast<std::size_t>(j)];
    for (int j = 0; j < row_block; ++j) vb[j] = b[e + static_cast<std::size_t>(j)];
    for (int j = 0; j < row_block; ++j) vc[j] = c[e + static_cast<std::size_t>(j)];
    for (int j = 0; j < row_block; ++j) o[j] = apply3<op>(va[j], vb[j], vc[j]);
    for (int j = 0; j < row_block; ++j) out[e + static_cast<std::size_t>(j)] = o[j];
  }
  for (; e < total; ++e) out[e] = apply3<op>(a[e], b[e], c[e]);
}

// Const: the row's constant (literal or column) broadcast over the lanes.
template <Kind KK, int L, bool Ind>
void k_konst(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  load_impl<KK, L, Ind>(s.konst, ctx, r0, idx, n, out);
}

// Input: values[(base + r)·L + l] = state[ordinal(r)·B + b0 + l].
template <int L, bool Ind>
void k_input(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  const int LL = lanes<L>(ctx);
  const std::size_t LLs = static_cast<std::size_t>(LL);
  const double* state = ctx.state + ctx.b0;
  const std::size_t B = static_cast<std::size_t>(ctx.B);
  for (int i = 0; i < n; ++i) {
    const double* src = state + static_cast<std::size_t>(s.ordinal[row_at<Ind>(r0, idx, i)]) * B;
    double* dst = out + static_cast<std::size_t>(i) * LLs;
    for (int l = 0; l < LL; ++l) dst[l] = src[l];
  }
}

// ---- segments ------------------------------------------------------------------------------

// A group of up to RJ rows of one bucket (rows i .. i + nj of a block of n rows, all with `len`
// members, transposed as memT[k·n + row]) folded with local accumulators. L > 1: one L-lane
// accumulator per row. The fold order per row is member 0 (Sum) or c_0 (Affine), then members
// 1, 2, ... exactly as recorded.
template <bool Affine, int L, int RJ>
EPYKOS_EXEC_INLINE void seg_group(int i, int nj, int n, int len, const double* values, const std::int32_t* rows,
                      const std::int32_t* memT, const double* coefT, const double* konst, bool konst_is_column,
                      double* dom) {
  static_assert(L > 1);
  double acc[RJ][L];
  int k0;
  if constexpr (Affine) {
    for (int j = 0; j < nj; ++j) {
      const double v = konst_is_column ? konst[rows[i + j]] : konst[0];
      for (int l = 0; l < L; ++l) acc[j][l] = v;
    }
    k0 = 0;
  } else {
    for (int j = 0; j < nj; ++j) {
      const double* src = values + static_cast<std::size_t>(memT[i + j]) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) acc[j][l] = src[l];
    }
    k0 = 1;
  }
  for (int k = k0; k < len; ++k) {
    const std::int32_t* mk = memT + static_cast<std::size_t>(k) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i);
    for (int j = 0; j < nj; ++j) {
      const double* src = values + static_cast<std::size_t>(mk[j]) * static_cast<std::size_t>(L);
      if constexpr (Affine) {
        const double c = coefT[static_cast<std::size_t>(k) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i + j)];
        for (int l = 0; l < L; ++l) {
          const double p = c * src[l];
          acc[j][l] = acc[j][l] + p;
        }
      } else {
        for (int l = 0; l < L; ++l) acc[j][l] = acc[j][l] + src[l];
      }
    }
  }
  for (int j = 0; j < nj; ++j) {
    double* dst = dom + static_cast<std::size_t>(rows[i + j]) * static_cast<std::size_t>(L);
    for (int l = 0; l < L; ++l) dst[l] = acc[j][l];
  }
}

// L = 1: a group of up to RJ rows, the row axis vectorised.
template <bool Affine, int RJ>
EPYKOS_EXEC_INLINE void seg_group1(int i, int nj, int n, int len, const double* values, const std::int32_t* rows,
                       const std::int32_t* memT, const double* coefT, const double* konst, bool konst_is_column,
                       double* dom) {
  double acc[RJ];
  int k0;
  if constexpr (Affine) {
    for (int j = 0; j < nj; ++j) acc[j] = konst_is_column ? konst[rows[i + j]] : konst[0];
    k0 = 0;
  } else {
    for (int j = 0; j < nj; ++j) acc[j] = values[memT[i + j]];
    k0 = 1;
  }
  for (int k = k0; k < len; ++k) {
    const std::size_t kk = static_cast<std::size_t>(k) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i);
    const std::int32_t* mk = memT + kk;
    double v[RJ];
    for (int j = 0; j < nj; ++j) v[j] = values[mk[j]];
    if constexpr (Affine) {
      const double* ck = coefT + kk;
      for (int j = 0; j < nj; ++j) {
        const double p = ck[j] * v[j];
        acc[j] = acc[j] + p;
      }
    } else {
      for (int j = 0; j < nj; ++j) acc[j] = acc[j] + v[j];
    }
  }
  for (int j = 0; j < nj; ++j) dom[rows[i + j]] = acc[j];
}

// A block whose members are all in the value buffer: rows in flight in local accumulators
// (L = 0: the shared accumulator scratch, plain loops).
template <bool Affine, int L>
inline void seg_block_gathered(const SegPlan& sp, const SegBlock& blk, const RunCtx& ctx, double* dom) {
  const double* values = ctx.values;
  const double* konst = Affine ? sp.konst.data : nullptr;
  const bool konst_is_column = Affine && sp.konst.kind == Kind::Col;
  const int n = blk.n;
  const int len = blk.len;
  const std::int32_t* rows = sp.rows.data() + blk.row0;
  const std::int32_t* memT = sp.memT.data() + blk.memT;
  const double* coefT = Affine ? sp.coefT.data() + blk.memT : nullptr;
  if constexpr (L == 1) {
    constexpr int RJ = seg_rows_in_flight<1>;
    int i = 0;
    for (; i + RJ <= n; i += RJ) seg_group1<Affine, RJ>(i, RJ, n, len, values, rows, memT, coefT, konst, konst_is_column, dom);
    if (i < n) seg_group1<Affine, RJ>(i, n - i, n, len, values, rows, memT, coefT, konst, konst_is_column, dom);
  } else if constexpr (L > 1) {
    constexpr int RJ = seg_rows_in_flight<L>;
    int i = 0;
    for (; i + RJ <= n; i += RJ) seg_group<Affine, L, RJ>(i, RJ, n, len, values, rows, memT, coefT, konst, konst_is_column, dom);
    if (i < n) seg_group<Affine, L, RJ>(i, n - i, n, len, values, rows, memT, coefT, konst, konst_is_column, dom);
  } else {
    const std::size_t LL = static_cast<std::size_t>(ctx.L);
    double* acc = ctx.acc;
    int k0;
    if constexpr (Affine) {
      for (int i = 0; i < n; ++i) {
        const double v = konst_is_column ? konst[rows[i]] : konst[0];
        for (std::size_t l = 0; l < LL; ++l) acc[static_cast<std::size_t>(i) * LL + l] = v;
      }
      k0 = 0;
    } else {
      for (int i = 0; i < n; ++i) {
        const double* src = values + static_cast<std::size_t>(memT[i]) * LL;
        for (std::size_t l = 0; l < LL; ++l) acc[static_cast<std::size_t>(i) * LL + l] = src[l];
      }
      k0 = 1;
    }
    for (int k = k0; k < len; ++k) {
      const std::size_t kk = static_cast<std::size_t>(k) * static_cast<std::size_t>(n);
      for (int i = 0; i < n; ++i) {
        const double* src = values + static_cast<std::size_t>(memT[kk + static_cast<std::size_t>(i)]) * LL;
        double* d = acc + static_cast<std::size_t>(i) * LL;
        if constexpr (Affine) {
          const double c = coefT[kk + static_cast<std::size_t>(i)];
          for (std::size_t l = 0; l < LL; ++l) {
            const double p = c * src[l];
            d[l] = d[l] + p;
          }
        } else {
          for (std::size_t l = 0; l < LL; ++l) d[l] = d[l] + src[l];
        }
      }
    }
    for (int i = 0; i < n; ++i) {
      double* d = dom + static_cast<std::size_t>(rows[i]) * LL;
      const double* src = acc + static_cast<std::size_t>(i) * LL;
      for (std::size_t l = 0; l < LL; ++l) d[l] = src[l];
    }
  }
}

// ---- reduction epilogues ------------------------------------------------------------------
//
// A fused block's members are never stored: the producer's last step is evaluated with an
// epilogue that folds each value into its row's accumulator, with several rows in flight in
// local accumulators (32 lanes: eight 4-wide accumulators at every L; one row at L >= 32) and
// the member positions of a producer run as the inner loop. The value v of a member is computed
// exactly as the plain kernel computes it (same operation, same operands), then folded: Sum
// acc = acc + v (acc = v at the block's first position); Affine p = c·v, acc = acc + p — the
// recorded fold order, one IEEE rounding per operation, contraction off in the including TU.

template <int L>
inline constexpr int acc_rows_in_flight = acc_rows_in_flight_for(L == 0 ? 1 : L);

// L = 1: RJ rows from i0 of a run of kr positions; f(r, t) is the member's value for domain
// row r at tile row t.
template <bool Affine, bool Ind, int RJ, class F>
EPYKOS_EXEC_INLINE void acc_rows1(F&& f, int r0, const std::int32_t* idx, int n, int i0, int kr, bool first, const double* coef,
                      double* accbuf) {
  double acc[RJ];
  if (Affine || !first) {
    for (int j = 0; j < RJ; ++j) acc[j] = accbuf[i0 + j];
  } else {
    for (int j = 0; j < RJ; ++j) acc[j] = 0.0;  // overwritten at position 0
  }
  for (int k = 0; k < kr; ++k) {
    const int t0 = k * n + i0;
    double v[RJ];
    for (int j = 0; j < RJ; ++j) v[j] = f(row_at<Ind>(r0, idx, t0 + j), t0 + j);
    if constexpr (Affine) {
      const double* ck = coef + t0;
      for (int j = 0; j < RJ; ++j) {
        const double p = ck[j] * v[j];
        acc[j] = acc[j] + p;
      }
    } else if (first && k == 0) {
      for (int j = 0; j < RJ; ++j) acc[j] = v[j];
    } else {
      for (int j = 0; j < RJ; ++j) acc[j] = acc[j] + v[j];
    }
  }
  for (int j = 0; j < RJ; ++j) accbuf[i0 + j] = acc[j];
}

// L > 1: f(r, t, v) fills the member's L lanes.
template <bool Affine, bool Ind, int L, int RJ, class F>
EPYKOS_EXEC_INLINE void acc_rowsL(F&& f, int r0, const std::int32_t* idx, int n, int i0, int kr, bool first, const double* coef,
                      double* accbuf) {
  double acc[RJ][L];
  if (Affine || !first) {
    for (int j = 0; j < RJ; ++j) {
      const double* src = accbuf + static_cast<std::size_t>(i0 + j) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) acc[j][l] = src[l];
    }
  } else {
    for (int j = 0; j < RJ; ++j) {
      for (int l = 0; l < L; ++l) acc[j][l] = 0.0;  // overwritten at position 0
    }
  }
  for (int k = 0; k < kr; ++k) {
    const int t0 = k * n + i0;
    for (int j = 0; j < RJ; ++j) {
      double v[L];
      f(row_at<Ind>(r0, idx, t0 + j), t0 + j, v);
      if constexpr (Affine) {
        const double c = coef[t0 + j];
        for (int l = 0; l < L; ++l) {
          const double p = c * v[l];
          acc[j][l] = acc[j][l] + p;
        }
      } else if (first && k == 0) {
        for (int l = 0; l < L; ++l) acc[j][l] = v[l];
      } else {
        for (int l = 0; l < L; ++l) acc[j][l] = acc[j][l] + v[l];
      }
    }
  }
  for (int j = 0; j < RJ; ++j) {
    double* dst = accbuf + static_cast<std::size_t>(i0 + j) * static_cast<std::size_t>(L);
    for (int l = 0; l < L; ++l) dst[l] = acc[j][l];
  }
}

// The rows left after the full groups (nj < RJ), in halving groups: every group has a
// compile-time row count, so the tail vectorises like the full groups.
template <bool Affine, bool Ind, int RJ, class F>
EPYKOS_EXEC_INLINE void acc_tail1(F&& f, int r0, const std::int32_t* idx, int n, int i0, int nj, int kr, bool first,
                      const double* coef, double* accbuf) {
  if constexpr (RJ > 1) {
    constexpr int H = RJ / 2;
    if (nj >= H) {
      acc_rows1<Affine, Ind, H>(f, r0, idx, n, i0, kr, first, coef, accbuf);
      i0 += H;
      nj -= H;
    }
    acc_tail1<Affine, Ind, H>(f, r0, idx, n, i0, nj, kr, first, coef, accbuf);
  } else {
    (void)f; (void)r0; (void)idx; (void)n; (void)i0; (void)nj; (void)kr; (void)first; (void)coef; (void)accbuf;
  }
}

template <bool Affine, bool Ind, int L, int RJ, class F>
EPYKOS_EXEC_INLINE void acc_tailL(F&& f, int r0, const std::int32_t* idx, int n, int i0, int nj, int kr, bool first,
                      const double* coef, double* accbuf) {
  if constexpr (RJ > 1) {
    constexpr int H = RJ / 2;
    if (nj >= H) {
      acc_rowsL<Affine, Ind, L, H>(f, r0, idx, n, i0, kr, first, coef, accbuf);
      i0 += H;
      nj -= H;
    }
    acc_tailL<Affine, Ind, L, H>(f, r0, idx, n, i0, nj, kr, first, coef, accbuf);
  } else {
    (void)f; (void)r0; (void)idx; (void)n; (void)i0; (void)nj; (void)kr; (void)first; (void)coef; (void)accbuf;
  }
}

// Runtime lane count: in place on the accumulator, plain loops; f(r, t, l) is one lane.
template <bool Affine, bool Ind, class F>
EPYKOS_EXEC_INLINE void acc_rows0(F&& f, int r0, const std::int32_t* idx, int n, int LL, int kr, bool first, const double* coef,
                      double* accbuf) {
  const std::size_t LLs = static_cast<std::size_t>(LL);
  for (int k = 0; k < kr; ++k) {
    for (int i = 0; i < n; ++i) {
      const int t = k * n + i;
      const int r = row_at<Ind>(r0, idx, t);
      double* a = accbuf + static_cast<std::size_t>(i) * LLs;
      for (int l = 0; l < LL; ++l) {
        const double v = f(r, t, l);
        if constexpr (Affine) {
          const double p = coef[t] * v;
          a[l] = a[l] + p;
        } else if (first && k == 0) {
          a[l] = v;
        } else {
          a[l] = a[l] + v;
        }
      }
    }
  }
}

template <bool Affine, bool Ind, int L, class F1, class FL, class F0>
EPYKOS_EXEC_INLINE void acc_run(F1&& f1, FL&& fL, F0&& f0, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, int kr,
                    bool first, const double* coef, double* acc) {
  (void)f1; (void)fL; (void)f0; (void)ctx;
  if constexpr (L == 1) {
    constexpr int RJ = acc_rows_in_flight<1>;
    int i = 0;
    for (; i + RJ <= n; i += RJ) acc_rows1<Affine, Ind, RJ>(f1, r0, idx, n, i, kr, first, coef, acc);
    if (i < n) acc_tail1<Affine, Ind, RJ>(f1, r0, idx, n, i, n - i, kr, first, coef, acc);
  } else if constexpr (L > 1) {
    constexpr int RJ = acc_rows_in_flight<L>;
    int i = 0;
    for (; i + RJ <= n; i += RJ) acc_rowsL<Affine, Ind, L, RJ>(fL, r0, idx, n, i, kr, first, coef, acc);
    if (i < n) acc_tailL<Affine, Ind, L, RJ>(fL, r0, idx, n, i, n - i, kr, first, coef, acc);
  } else {
    acc_rows0<Affine, Ind>(f0, r0, idx, n, ctx.L, kr, first, coef, acc);
  }
}

template <Op op, Kind KA, Kind KB, int L, bool Affine>
void k_binary_acc(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, int kr, bool first,
                  const double* coef, double* acc) {
  const double* values = ctx.values;
  const Operand& a = s.a;
  const Operand& b = s.b;
  const std::size_t LL = static_cast<std::size_t>(lanes<L>(ctx));
  auto f1 = [&](int r, int t) {
    return apply2<op>(elem<KA>(values, a, r, static_cast<std::size_t>(t), 0, 1),
                      elem<KB>(values, b, r, static_cast<std::size_t>(t), 0, 1));
  };
  auto fL = [&](int r, int t, double* v) {
    if constexpr (L > 1) {
      double va[L], vb[L];
      const double sa = stage_row<KA, L>(va, values, a, r, static_cast<std::size_t>(t));
      const double sb = stage_row<KB, L>(vb, values, b, r, static_cast<std::size_t>(t));
      for (int l = 0; l < L; ++l) v[l] = apply2<op>(lane<KA>(sa, va, l), lane<KB>(sb, vb, l));
    } else {
      (void)r; (void)t; (void)v;
    }
  };
  auto f0 = [&](int r, int t, int l) {
    return apply2<op>(elem<KA>(values, a, r, static_cast<std::size_t>(t), l, LL),
                      elem<KB>(values, b, r, static_cast<std::size_t>(t), l, LL));
  };
  acc_run<Affine, false, L>(f1, fL, f0, ctx, r0, idx, n, kr, first, coef, acc);
}

template <Op op, Kind KA, int L, bool Affine>
void k_unary_acc(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, int kr, bool first,
                 const double* coef, double* acc) {
  const double* values = ctx.values;
  const Operand& a = s.a;
  const std::size_t LL = static_cast<std::size_t>(lanes<L>(ctx));
  auto f1 = [&](int r, int t) { return apply1<op>(elem<KA>(values, a, r, static_cast<std::size_t>(t), 0, 1)); };
  auto fL = [&](int r, int t, double* v) {
    if constexpr (L > 1) {
      double va[L];
      const double sa = stage_row<KA, L>(va, values, a, r, static_cast<std::size_t>(t));
      for (int l = 0; l < L; ++l) v[l] = apply1<op>(lane<KA>(sa, va, l));
    } else {
      (void)r; (void)t; (void)v;
    }
  };
  auto f0 = [&](int r, int t, int l) { return apply1<op>(elem<KA>(values, a, r, static_cast<std::size_t>(t), l, LL)); };
  acc_run<Affine, false, L>(f1, fL, f0, ctx, r0, idx, n, kr, first, coef, acc);
}

// The member is an operand as it stands: a Const step's literal / column (indirect rows), the
// members of a materialised domain (Gat over the block's member ids, contiguous rows) or the
// member buffer (Vec) holding a last step that has no epilogue of its own.
template <Kind K, int L, bool Affine, bool Ind>
void k_load_acc(const Operand& o, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, int kr, bool first,
                const double* coef, double* acc) {
  const double* values = ctx.values;
  const std::size_t LL = static_cast<std::size_t>(lanes<L>(ctx));
  auto f1 = [&](int r, int t) { return elem<K>(values, o, r, static_cast<std::size_t>(t), 0, 1); };
  auto fL = [&](int r, int t, double* v) {
    if constexpr (L > 1) {
      double va[L];
      const double sa = stage_row<K, L>(va, values, o, r, static_cast<std::size_t>(t));
      for (int l = 0; l < L; ++l) v[l] = lane<K>(sa, va, l);
    } else {
      (void)r; (void)t; (void)v;
    }
  };
  auto f0 = [&](int r, int t, int l) { return elem<K>(values, o, r, static_cast<std::size_t>(t), l, LL); };
  acc_run<Affine, Ind, L>(f1, fL, f0, ctx, r0, idx, n, kr, first, coef, acc);
}

template <Kind KK, int L, bool Affine>
void k_konst_acc(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, int kr, bool first,
                 const double* coef, double* acc) {
  k_load_acc<KK, L, Affine, false>(s.konst, ctx, r0, idx, n, kr, first, coef, acc);
}

// A block with fused producers: for each chunk of kc member positions and each run of positions
// with the same producer, the producer's earlier steps are evaluated over the run's member rows
// (indirect row mode, tile row kk·n + i) into their scratch and its last step through its
// epilogue into the accumulator; a last step without an epilogue goes through the member buffer;
// members of a materialised domain are folded straight from the value buffer.
template <bool Affine, int L>
inline void seg_block_fused(const SegPlan& sp, const SegBlock& blk, const RunCtx& ctx, double* dom) {
  const int LL = lanes<L>(ctx);
  const std::size_t LLs = static_cast<std::size_t>(LL);
  const int v = ctx.v;
  const int n = blk.n;
  const int len = blk.len;
  const int kc = blk.kc;
  const std::int32_t* rows = sp.rows.data() + blk.row0;
  const std::int32_t* memT = sp.memT.data() + blk.memT;
  const double* coefT = Affine ? sp.coefT.data() + blk.memT : nullptr;
  const std::int32_t* prod = sp.prod.data() + blk.prod;
  double* acc = ctx.acc;
  if constexpr (Affine) {
    if (sp.konst.kind == Kind::Col) {
      load_impl<Kind::Col, L, true>(sp.konst, ctx, 0, rows, n, acc);
    } else {
      load_impl<Kind::Lit, L, false>(sp.konst, ctx, 0, nullptr, n, acc);
    }
  }
  for (int k0 = 0; k0 < len; k0 += kc) {
    const int k1 = std::min(len, k0 + kc);
    for (int k = k0; k < k1;) {
      int ke = k + 1;
      while (ke < k1 && prod[ke] == prod[k]) ++ke;
      const int kr = ke - k;
      const std::int32_t* mem = memT + static_cast<std::size_t>(k) * static_cast<std::size_t>(n);
      const double* coef = Affine ? coefT + static_cast<std::size_t>(k) * static_cast<std::size_t>(n) : nullptr;
      const bool first = !Affine && k == 0;
      if (prod[k] >= 0) {
        const FusedProducer& fp = sp.producers[static_cast<std::size_t>(prod[k])];
        const int n_rows = kr * n;
        if (fp.block_mode) {
          // The producer's operands are laid out in this reduction's member order: contiguous
          // rows from the run's offset.
          const int r0 = sp.prod_off[blk.prod + static_cast<std::size_t>(k)];
          const std::size_t n_steps = fp.steps.size();
          for (std::size_t j = 0; j < n_steps; ++j) {
            const StepPlan& s = fp.steps[j];
            for (int q = 0; q < s.n_pre; ++q) {
              s.pre[q].fn[row_contiguous][v](s.pre[q].src, ctx, r0, nullptr, n_rows, s.pre[q].dst);
            }
            if (j + 1 < n_steps) {
              s.fn[row_contiguous][v](s, ctx, r0, nullptr, n_rows, s.out);
            } else if (s.acc_fn[Affine ? 1 : 0][v] != nullptr) {
              s.acc_fn[Affine ? 1 : 0][v](s, ctx, r0, nullptr, n, kr, first, coef, acc);
            } else {
              s.fn[row_contiguous][v](s, ctx, r0, nullptr, n_rows, ctx.member);
              Operand o;
              o.kind = Kind::Vec;
              o.data = ctx.member;
              sp.load_acc_vec[v](o, ctx, 0, nullptr, n, kr, first, coef, acc);
            }
          }
        } else {
          // Not re-laid out: the producer's own steps over the member ids (indirect rows) into
          // the member buffer.
          const GroupPlan& g = ctx.groups[fp.domain];
          eval_group(g, ctx, -g.value_base, mem, n_rows, ctx.member);
          Operand o;
          o.kind = Kind::Vec;
          o.data = ctx.member;
          sp.load_acc_vec[v](o, ctx, 0, nullptr, n, kr, first, coef, acc);
        }
      } else {
        Operand o;
        o.kind = Kind::Gat;
        o.data = ctx.values;
        o.index = mem;
        sp.load_acc_gat[v](o, ctx, 0, nullptr, n, kr, first, coef, acc);
      }
      k = ke;
    }
  }
  for (int i = 0; i < n; ++i) {
    double* d = dom + static_cast<std::size_t>(rows[i]) * LLs;
    const double* src = acc + static_cast<std::size_t>(i) * LLs;
    for (std::size_t l = 0; l < LLs; ++l) d[l] = src[l];
  }
}

// Whole-domain Sum / Affine over length buckets with transposed members (see plan.hpp).
template <bool Affine, int L>
void k_seg_whole(const SegPlan& sp, const RunCtx& ctx, double* dom) {
  for (const SegBlock& blk : sp.blocks) {
    if (blk.fused) {
      seg_block_fused<Affine, L>(sp, blk, ctx, dom);
    } else {
      seg_block_gathered<Affine, L>(sp, blk, ctx, dom);
    }
  }
}

// A whole-domain Sum / Affine group inlined into a consumer (plan.hpp: InlinedProducer), evaluated
// for consumer rows r0 .. r0 + n into out[i·L + l] from its consumer-ordered transposed tables:
// every row has `len` members, fold step k reads memT[k·rows + r0 + i] — the access pattern of
// the bucketed whole-domain pass, rows independent, each row's fold the recorded left-to-right
// order (E0). L = 1: blocks of rows, the row axis vectorised; L > 1: a row's L lanes in locals.
template <bool Affine, int L>
void k_seg_list(const InlinedProducer& ip, const RunCtx& ctx, int r0, int n, double* out) {
  const double* values = ctx.values;
  const int len = ip.len;
  const std::size_t R = static_cast<std::size_t>(ip.rows);
  const std::int32_t* memT = ip.memT.data() + static_cast<std::size_t>(r0);
  const double* coefT = Affine ? ip.coefT.data() + static_cast<std::size_t>(r0) : nullptr;
  const double* konst = Affine ? ip.konst.data : nullptr;
  const bool konst_col = Affine && ip.konst.kind == Kind::Col;
  auto row_c0 = [&](int i) { return konst_col ? konst[r0 + i] : konst[0]; };
  if constexpr (L == 1) {
    constexpr int RB = row_block;
    int i = 0;
    for (; i + RB <= n; i += RB) {
      double acc[RB];
      int k0;
      if constexpr (Affine) {
        for (int j = 0; j < RB; ++j) acc[j] = row_c0(i + j);
        k0 = 0;
      } else {
        for (int j = 0; j < RB; ++j) acc[j] = values[memT[i + j]];
        k0 = 1;
      }
      for (int k = k0; k < len; ++k) {
        const std::size_t kk = static_cast<std::size_t>(k) * R + static_cast<std::size_t>(i);
        double v[RB];
        for (int j = 0; j < RB; ++j) v[j] = values[memT[kk + static_cast<std::size_t>(j)]];
        if constexpr (Affine) {
          for (int j = 0; j < RB; ++j) {
            const double p = coefT[kk + static_cast<std::size_t>(j)] * v[j];
            acc[j] = acc[j] + p;
          }
        } else {
          for (int j = 0; j < RB; ++j) acc[j] = acc[j] + v[j];
        }
      }
      for (int j = 0; j < RB; ++j) out[i + j] = acc[j];
    }
    for (; i < n; ++i) {
      double acc;
      int k0;
      if constexpr (Affine) {
        acc = row_c0(i);
        k0 = 0;
      } else {
        acc = values[memT[i]];
        k0 = 1;
      }
      for (int k = k0; k < len; ++k) {
        const std::size_t kk = static_cast<std::size_t>(k) * R + static_cast<std::size_t>(i);
        const double v = values[memT[kk]];
        if constexpr (Affine) {
          const double p = coefT[kk] * v;
          acc = acc + p;
        } else {
          acc = acc + v;
        }
      }
      out[i] = acc;
    }
  } else if constexpr (L > 1) {
    for (int i = 0; i < n; ++i) {
      double acc[L];
      int k0;
      if constexpr (Affine) {
        const double c0 = row_c0(i);
        for (int l = 0; l < L; ++l) acc[l] = c0;
        k0 = 0;
      } else {
        const double* src = values + static_cast<std::size_t>(memT[i]) * static_cast<std::size_t>(L);
        for (int l = 0; l < L; ++l) acc[l] = src[l];
        k0 = 1;
      }
      for (int k = k0; k < len; ++k) {
        const std::size_t kk = static_cast<std::size_t>(k) * R + static_cast<std::size_t>(i);
        const double* src = values + static_cast<std::size_t>(memT[kk]) * static_cast<std::size_t>(L);
        if constexpr (Affine) {
          const double c = coefT[kk];
          for (int l = 0; l < L; ++l) {
            const double p = c * src[l];
            acc[l] = acc[l] + p;
          }
        } else {
          for (int l = 0; l < L; ++l) acc[l] = acc[l] + src[l];
        }
      }
      double* dst = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) dst[l] = acc[l];
    }
  } else {
    const std::size_t LL = static_cast<std::size_t>(ctx.L);
    for (int i = 0; i < n; ++i) {
      double* d = out + static_cast<std::size_t>(i) * LL;
      int k0;
      if constexpr (Affine) {
        const double c0 = row_c0(i);
        for (std::size_t l = 0; l < LL; ++l) d[l] = c0;
        k0 = 0;
      } else {
        const double* src = values + static_cast<std::size_t>(memT[i]) * LL;
        for (std::size_t l = 0; l < LL; ++l) d[l] = src[l];
        k0 = 1;
      }
      for (int k = k0; k < len; ++k) {
        const std::size_t kk = static_cast<std::size_t>(k) * R + static_cast<std::size_t>(i);
        const double* src = values + static_cast<std::size_t>(memT[kk]) * LL;
        if constexpr (Affine) {
          const double c = coefT[kk];
          for (std::size_t l = 0; l < LL; ++l) {
            const double p = c * src[l];
            d[l] = d[l] + p;
          }
        } else {
          for (std::size_t l = 0; l < LL; ++l) d[l] = d[l] + src[l];
        }
      }
    }
  }
}

// Per-tile Sum / Affine, row by row (a Sum / Affine step that shares its group with other steps).
template <bool Affine, int L, bool Ind>
void k_seg_rows(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  const int LL = lanes<L>(ctx);
  const std::size_t LLs = static_cast<std::size_t>(LL);
  const SegPlan& sp = *s.seg;
  const double* values = ctx.values;
  for (int i = 0; i < n; ++i) {
    const int r = row_at<Ind>(r0, idx, i);
    const std::int32_t lo = sp.offsets[r];
    const std::int32_t hi = sp.offsets[r + 1];
    double* o = out + static_cast<std::size_t>(i) * LLs;
    for (int l = 0; l < LL; ++l) {
      double acc;
      std::int32_t m = lo;
      if constexpr (Affine) {
        acc = sp.konst.kind == Kind::Col ? sp.konst.data[r] : sp.konst.data[0];
      } else {
        acc = values[static_cast<std::size_t>(sp.members[lo]) * LLs + static_cast<std::size_t>(l)];
        m = lo + 1;
      }
      for (; m < hi; ++m) {
        const double v = values[static_cast<std::size_t>(sp.members[m]) * LLs + static_cast<std::size_t>(l)];
        if constexpr (Affine) {
          const double p = sp.coefs[m] * v;
          acc = acc + p;
        } else {
          acc = acc + v;
        }
      }
      o[l] = acc;
    }
  }
}

// ---- fused pairs ---------------------------------------------------------------------------
//
// Two consecutive IR steps t = op(x, y); v = op2(t, z) (or op2(z, t)) whose middle value t has
// no other reader are one kernel: per row and lane, both operations are applied in sequence in
// registers — the same two IEEE roundings the scalar evaluator performs, in the same order,
// contraction off — and only v is stored (plain kernel) or folded (reduction epilogue). x, y, z
// are row-addressed: a per-row scalar (a literal or a column, read as data[row · stride]) or a
// gathered row of the value buffer; an earlier step's scratch never enters a pair, so a longer
// chain is a pair followed by single steps. The plan (interpreter.cpp: pair_of) swaps the
// operands of a commutative op so that a scalar comes first, which is why (S, G), (G, S) and
// (G, G) are the shapes below; Neg takes one gathered operand.

constexpr bool pair_unary(Op op) noexcept { return op == Op::Neg; }

// L = 1 / runtime L: one element of a pair operand.
template <PK K>
EPYKOS_EXEC_INLINE double pget(const double* values, const Operand& o, int r, int l, std::size_t LL) noexcept {
  (void)values;
  if constexpr (K == PK::S) {
    (void)l; (void)LL;
    return o.data[static_cast<std::size_t>(r) * static_cast<std::size_t>(o.stride)];
  } else if constexpr (K == PK::G) {
    return o.data[static_cast<std::size_t>(o.index[r]) * LL + static_cast<std::size_t>(l)];
  } else {
    (void)o; (void)r; (void)l; (void)LL;
    return 0.0;
  }
}

// L > 1: a row's view of a pair operand — the scalar, or the base of the gathered row's lanes.
template <PK K>
struct PairRow {
  double s = 0.0;
  const double* p = nullptr;
  EPYKOS_EXEC_INLINE static PairRow make(const double* values, const Operand& o, int r, std::size_t LL) noexcept {
    (void)values;
    PairRow v;
    if constexpr (K == PK::S) {
      (void)LL;
      v.s = o.data[static_cast<std::size_t>(r) * static_cast<std::size_t>(o.stride)];
    } else if constexpr (K == PK::G) {
      v.p = o.data + static_cast<std::size_t>(o.index[r]) * LL;
    } else {
      (void)o; (void)r; (void)LL;
    }
    return v;
  }
  EPYKOS_EXEC_INLINE double get(int l) const noexcept {
    if constexpr (K == PK::G) {
      return p[l];
    } else {
      (void)l;
      return s;
    }
  }
};

template <Op op, Op op2, bool PR>
EPYKOS_EXEC_INLINE double pair_eval(double x, double y, double z) noexcept {
  double t;
  if constexpr (pair_unary(op)) {
    (void)y;
    t = apply1<op>(x);
  } else {
    t = apply2<op>(x, y);
  }
  if constexpr (pair_unary(op2)) {
    (void)z;
    return apply1<op2>(t);
  } else if constexpr (PR) {
    return apply2<op2>(z, t);
  } else {
    return apply2<op2>(t, z);
  }
}

// ---- chain tails ---------------------------------------------------------------------------
//
// A pair's value may feed a libm step of the group (Exp / Log) that nothing else reads. The plain
// pair kernels apply it in place on the row (row block at L = 1) they have just stored, while it
// is still in L1: the libm call runs per element either way, but the argument tile is never
// written to and read back from the scratch. The same call on the same value as the single-step
// kernel (E0); Exp follows the plan's exp mode (std::exp, or exp_poly under ExpMode::poly, E1).
// Applied to the stored row rather than to the kernel's local array: with the calls in the same
// basic block as the locals, the compiler kept the locals in registers across them and
// scalarised the pair's own lane loop (measured). The block length is a compile-time constant
// where the row shape is (L lanes, or a row block at L = 1).

struct TailSet {
  int n = 0;
  Op op[StepPlan::max_tail] = {};
  bool poly = false;
  static TailSet of(const StepPlan& s) noexcept {
    TailSet t;
    t.n = s.n_tail;
    t.poly = s.tail_poly;
    for (int j = 0; j < StepPlan::max_tail; ++j) t.op[j] = s.tail_op[j];
    return t;
  }
};

// nb values o[0..nb) in place (NB > 0: nb is NB).
template <int NB>
EPYKOS_EXEC_INLINE void apply_tails(const TailSet& t, double* o, int nb) noexcept {
  const int n = NB > 0 ? NB : nb;
  for (int j = 0; j < t.n; ++j) {
    if (t.op[j] == Op::Exp) {
      if (t.poly) {
        for (int q = 0; q < n; ++q) o[q] = hand::exp_poly(o[q]);
      } else {
        for (int q = 0; q < n; ++q) o[q] = apply1<Op::Exp>(o[q]);
      }
    } else {
      for (int q = 0; q < n; ++q) o[q] = apply1<Op::Log>(o[q]);
    }
  }
}

// The pair's rows; Tail: the chain tail applied in place after each row / row block is stored.
template <Op op, PK KA, PK KB, Op op2, PK KC, bool PR, int L, bool Ind, bool Tail>
EPYKOS_EXEC_INLINE void pair_rows(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  const double* values = ctx.values;
  const Operand& a = s.a;
  const Operand& b = s.b;
  const Operand& c = s.c;
  const TailSet ts = Tail ? TailSet::of(s) : TailSet{};
  if constexpr (L == 1) {
    int i = 0;
    for (; i + row_block <= n; i += row_block) {
      double o[row_block];
      for (int j = 0; j < row_block; ++j) {
        const int r = row_at<Ind>(r0, idx, i + j);
        o[j] = pair_eval<op, op2, PR>(pget<KA>(values, a, r, 0, 1), pget<KB>(values, b, r, 0, 1), pget<KC>(values, c, r, 0, 1));
      }
      for (int j = 0; j < row_block; ++j) out[i + j] = o[j];
      if constexpr (Tail) apply_tails<row_block>(ts, out + i, row_block);
    }
    if (i < n) {
      const int i0 = i;
      for (; i < n; ++i) {
        const int r = row_at<Ind>(r0, idx, i);
        out[i] = pair_eval<op, op2, PR>(pget<KA>(values, a, r, 0, 1), pget<KB>(values, b, r, 0, 1), pget<KC>(values, c, r, 0, 1));
      }
      if constexpr (Tail) apply_tails<0>(ts, out + i0, n - i0);
    }
  } else if constexpr (L > 1) {
    for (int i = 0; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      const PairRow<KA> va = PairRow<KA>::make(values, a, r, L);
      const PairRow<KB> vb = PairRow<KB>::make(values, b, r, L);
      const PairRow<KC> vc = PairRow<KC>::make(values, c, r, L);
      double o[L];
      for (int l = 0; l < L; ++l) o[l] = pair_eval<op, op2, PR>(va.get(l), vb.get(l), vc.get(l));
      double* dst = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) dst[l] = o[l];
      if constexpr (Tail) apply_tails<L>(ts, dst, L);
    }
  } else {
    const std::size_t LL = static_cast<std::size_t>(ctx.L);
    for (int i = 0; i < n; ++i) {
      const int r = row_at<Ind>(r0, idx, i);
      double* dst = out + static_cast<std::size_t>(i) * LL;
      for (int l = 0; l < ctx.L; ++l) {
        dst[l] = pair_eval<op, op2, PR>(pget<KA>(values, a, r, l, LL), pget<KB>(values, b, r, l, LL), pget<KC>(values, c, r, l, LL));
      }
      if constexpr (Tail) apply_tails<0>(ts, dst, ctx.L);
    }
  }
}

template <Op op, PK KA, PK KB, Op op2, PK KC, bool PR, int L, bool Ind>
void k_pair(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out) {
  // Tails are applied in place on the row (row block) just stored — an L1 store-forward, not a
  // scratch round trip — rather than on the local array, which would make the compiler keep the
  // locals in registers across the tail's switch and scalarise the pair's own lane loop (measured).
  if (s.n_tail > 0) {
    pair_rows<op, KA, KB, op2, KC, PR, L, Ind, true>(s, ctx, r0, idx, n, out);
  } else {
    pair_rows<op, KA, KB, op2, KC, PR, L, Ind, false>(s, ctx, r0, idx, n, out);
  }
}

template <Op op, PK KA, PK KB, Op op2, PK KC, bool PR, int L, bool Affine>
void k_pair_acc(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, int kr, bool first,
                const double* coef, double* acc) {
  const double* values = ctx.values;
  const Operand& a = s.a;
  const Operand& b = s.b;
  const Operand& c = s.c;
  const std::size_t LL = static_cast<std::size_t>(lanes<L>(ctx));
  auto f1 = [&](int r, int) {
    return pair_eval<op, op2, PR>(pget<KA>(values, a, r, 0, 1), pget<KB>(values, b, r, 0, 1), pget<KC>(values, c, r, 0, 1));
  };
  auto fL = [&](int r, int, double* v) {
    if constexpr (L > 1) {
      const PairRow<KA> va = PairRow<KA>::make(values, a, r, L);
      const PairRow<KB> vb = PairRow<KB>::make(values, b, r, L);
      const PairRow<KC> vc = PairRow<KC>::make(values, c, r, L);
      for (int l = 0; l < L; ++l) v[l] = pair_eval<op, op2, PR>(va.get(l), vb.get(l), vc.get(l));
    } else {
      (void)r; (void)v;
    }
  };
  auto f0 = [&](int r, int, int l) {
    return pair_eval<op, op2, PR>(pget<KA>(values, a, r, l, LL), pget<KB>(values, b, r, l, LL), pget<KC>(values, c, r, l, LL));
  };
  acc_run<Affine, false, L>(f1, fL, f0, ctx, r0, idx, n, kr, first, coef, acc);
}

// ---- tables ----------------------------------------------------------------------------------

template <int L, bool Ind, Op op, Kind KA>
OpKernel pick_binary_b(Kind kb) {
  switch (kb) {
    case Kind::Vec: return &k_binary<op, KA, Kind::Vec, L, Ind>;
    case Kind::Lit: return &k_binary<op, KA, Kind::Lit, L, Ind>;
    case Kind::Col: return &k_binary<op, KA, Kind::Col, L, Ind>;
    case Kind::Gat: return &k_binary<op, KA, Kind::Gat, L, Ind>;
  }
  return nullptr;
}

template <int L, bool Ind, Op op>
OpKernel pick_binary_a(Kind ka, Kind kb) {
  switch (ka) {
    case Kind::Vec: return pick_binary_b<L, Ind, op, Kind::Vec>(kb);
    case Kind::Lit: return pick_binary_b<L, Ind, op, Kind::Lit>(kb);
    case Kind::Col: return pick_binary_b<L, Ind, op, Kind::Col>(kb);
    case Kind::Gat: return pick_binary_b<L, Ind, op, Kind::Gat>(kb);
  }
  return nullptr;
}

template <int L, bool Ind>
OpKernel pick_binary(Op op, Kind ka, Kind kb) {
  switch (op) {
    case Op::Add: return pick_binary_a<L, Ind, Op::Add>(ka, kb);
    case Op::Sub: return pick_binary_a<L, Ind, Op::Sub>(ka, kb);
    case Op::Mul: return pick_binary_a<L, Ind, Op::Mul>(ka, kb);
    case Op::Div: return pick_binary_a<L, Ind, Op::Div>(ka, kb);
    case Op::CmpLt: return pick_binary_a<L, Ind, Op::CmpLt>(ka, kb);
    case Op::CmpLe: return pick_binary_a<L, Ind, Op::CmpLe>(ka, kb);
    case Op::CmpGt: return pick_binary_a<L, Ind, Op::CmpGt>(ka, kb);
    case Op::CmpGe: return pick_binary_a<L, Ind, Op::CmpGe>(ka, kb);
    case Op::CmpEq: return pick_binary_a<L, Ind, Op::CmpEq>(ka, kb);
    default: throw std::invalid_argument("exec: not a binary op");
  }
}

template <int L>
OpKernel KernelTable<L>::binary(Op op, Kind ka, Kind kb, bool ind) {
  return ind ? pick_binary<L, true>(op, ka, kb) : pick_binary<L, false>(op, ka, kb);
}

template <int L, bool Ind, Op op>
OpKernel pick_unary_a(Kind ka) {
  switch (ka) {
    case Kind::Vec: return &k_unary<op, Kind::Vec, L, Ind>;
    case Kind::Lit: return &k_unary<op, Kind::Lit, L, Ind>;
    case Kind::Col: return &k_unary<op, Kind::Col, L, Ind>;
    case Kind::Gat: return &k_unary<op, Kind::Gat, L, Ind>;
  }
  return nullptr;
}

template <int L, bool Ind>
OpKernel pick_unary(Op op, Kind ka) {
  switch (op) {
    case Op::Neg: return pick_unary_a<L, Ind, Op::Neg>(ka);
    case Op::Exp: return pick_unary_a<L, Ind, Op::Exp>(ka);
    case Op::Log: return pick_unary_a<L, Ind, Op::Log>(ka);
    case Op::Sqrt: return pick_unary_a<L, Ind, Op::Sqrt>(ka);
    case Op::Recip: return pick_unary_a<L, Ind, Op::Recip>(ka);
    default: throw std::invalid_argument("exec: not a unary op");
  }
}

template <int L>
OpKernel KernelTable<L>::unary(Op op, Kind ka, bool ind) {
  return ind ? pick_unary<L, true>(op, ka) : pick_unary<L, false>(op, ka);
}

template <int L, bool Ind>
OpKernel pick_exp_poly(Kind ka) {
  switch (ka) {
    case Kind::Vec: return &k_exp_poly<Kind::Vec, L, Ind>;
    case Kind::Lit: return &k_exp_poly<Kind::Lit, L, Ind>;
    case Kind::Col: return &k_exp_poly<Kind::Col, L, Ind>;
    case Kind::Gat: return &k_exp_poly<Kind::Gat, L, Ind>;
  }
  return nullptr;
}

template <int L>
OpKernel KernelTable<L>::exp_poly(Kind ka, bool ind) {
  return ind ? pick_exp_poly<L, true>(ka) : pick_exp_poly<L, false>(ka);
}

template <int L>
OpKernel KernelTable<L>::ternary(Op op) {
  switch (op) {
    case Op::Fma: return &k_ternary<Op::Fma, L>;
    case Op::Select: return &k_ternary<Op::Select, L>;
    default: throw std::invalid_argument("exec: not a ternary op");
  }
}

template <int L, bool Ind>
OpKernel pick_konst(Kind kk) {
  switch (kk) {
    case Kind::Lit: return &k_konst<Kind::Lit, L, Ind>;
    case Kind::Col: return &k_konst<Kind::Col, L, Ind>;
    default: throw std::invalid_argument("exec: a Const step needs a literal or a column");
  }
}

template <int L>
OpKernel KernelTable<L>::konst(Kind kk, bool ind) {
  return ind ? pick_konst<L, true>(kk) : pick_konst<L, false>(kk);
}

template <int L>
OpKernel KernelTable<L>::input(bool ind) {
  return ind ? &k_input<L, true> : &k_input<L, false>;
}

template <int L>
OpKernel KernelTable<L>::seg_rows(bool affine, bool ind) {
  if (ind) return affine ? &k_seg_rows<true, L, true> : &k_seg_rows<false, L, true>;
  return affine ? &k_seg_rows<true, L, false> : &k_seg_rows<false, L, false>;
}

template <int L, bool Ind>
LoadKernel pick_load(Kind k) {
  switch (k) {
    case Kind::Vec: return &k_load<Kind::Vec, L, Ind>;
    case Kind::Lit: return &k_load<Kind::Lit, L, Ind>;
    case Kind::Col: return &k_load<Kind::Col, L, Ind>;
    case Kind::Gat: return &k_load<Kind::Gat, L, Ind>;
  }
  return nullptr;
}

template <int L>
LoadKernel KernelTable<L>::load(Kind k, bool ind) {
  return ind ? pick_load<L, true>(k) : pick_load<L, false>(k);
}

template <int L>
SegKernel KernelTable<L>::seg_whole(bool affine) {
  return affine ? &k_seg_whole<true, L> : &k_seg_whole<false, L>;
}

template <int L>
SegListKernel KernelTable<L>::seg_list(bool affine) {
  return affine ? &k_seg_list<true, L> : &k_seg_list<false, L>;
}

// ---- reduction epilogue tables -------------------------------------------------------------

template <int L, bool Affine, Op op, Kind KA>
AccKernel pick_binary_acc_b(Kind kb) {
  switch (kb) {
    case Kind::Vec: return &k_binary_acc<op, KA, Kind::Vec, L, Affine>;
    case Kind::Lit: return &k_binary_acc<op, KA, Kind::Lit, L, Affine>;
    case Kind::Col: return &k_binary_acc<op, KA, Kind::Col, L, Affine>;
    case Kind::Gat: return &k_binary_acc<op, KA, Kind::Gat, L, Affine>;
  }
  return nullptr;
}

template <int L, bool Affine, Op op>
AccKernel pick_binary_acc_a(Kind ka, Kind kb) {
  switch (ka) {
    case Kind::Vec: return pick_binary_acc_b<L, Affine, op, Kind::Vec>(kb);
    case Kind::Lit: return pick_binary_acc_b<L, Affine, op, Kind::Lit>(kb);
    case Kind::Col: return pick_binary_acc_b<L, Affine, op, Kind::Col>(kb);
    case Kind::Gat: return pick_binary_acc_b<L, Affine, op, Kind::Gat>(kb);
  }
  return nullptr;
}

template <int L, bool Affine>
AccKernel pick_binary_acc(Op op, Kind ka, Kind kb) {
  switch (op) {
    case Op::Add: return pick_binary_acc_a<L, Affine, Op::Add>(ka, kb);
    case Op::Sub: return pick_binary_acc_a<L, Affine, Op::Sub>(ka, kb);
    case Op::Mul: return pick_binary_acc_a<L, Affine, Op::Mul>(ka, kb);
    case Op::Div: return pick_binary_acc_a<L, Affine, Op::Div>(ka, kb);
    case Op::CmpLt: return pick_binary_acc_a<L, Affine, Op::CmpLt>(ka, kb);
    case Op::CmpLe: return pick_binary_acc_a<L, Affine, Op::CmpLe>(ka, kb);
    case Op::CmpGt: return pick_binary_acc_a<L, Affine, Op::CmpGt>(ka, kb);
    case Op::CmpGe: return pick_binary_acc_a<L, Affine, Op::CmpGe>(ka, kb);
    case Op::CmpEq: return pick_binary_acc_a<L, Affine, Op::CmpEq>(ka, kb);
    default: throw std::invalid_argument("exec: not a binary op");
  }
}

template <int L>
AccKernel KernelTable<L>::binary_acc(Op op, Kind ka, Kind kb, bool affine) {
  return affine ? pick_binary_acc<L, true>(op, ka, kb) : pick_binary_acc<L, false>(op, ka, kb);
}

template <int L, bool Affine, Op op>
AccKernel pick_unary_acc_a(Kind ka) {
  switch (ka) {
    case Kind::Vec: return &k_unary_acc<op, Kind::Vec, L, Affine>;
    case Kind::Lit: return &k_unary_acc<op, Kind::Lit, L, Affine>;
    case Kind::Col: return &k_unary_acc<op, Kind::Col, L, Affine>;
    case Kind::Gat: return &k_unary_acc<op, Kind::Gat, L, Affine>;
  }
  return nullptr;
}

template <int L, bool Affine>
AccKernel pick_unary_acc(Op op, Kind ka) {
  switch (op) {
    case Op::Neg: return pick_unary_acc_a<L, Affine, Op::Neg>(ka);
    case Op::Exp: return pick_unary_acc_a<L, Affine, Op::Exp>(ka);
    case Op::Log: return pick_unary_acc_a<L, Affine, Op::Log>(ka);
    case Op::Sqrt: return pick_unary_acc_a<L, Affine, Op::Sqrt>(ka);
    case Op::Recip: return pick_unary_acc_a<L, Affine, Op::Recip>(ka);
    default: throw std::invalid_argument("exec: not a unary op");
  }
}

template <int L>
AccKernel KernelTable<L>::unary_acc(Op op, Kind ka, bool affine) {
  return affine ? pick_unary_acc<L, true>(op, ka) : pick_unary_acc<L, false>(op, ka);
}

template <int L, bool Affine>
AccKernel pick_konst_acc(Kind kk) {
  switch (kk) {
    case Kind::Lit: return &k_konst_acc<Kind::Lit, L, Affine>;
    case Kind::Col: return &k_konst_acc<Kind::Col, L, Affine>;
    default: throw std::invalid_argument("exec: a Const step needs a literal or a column");
  }
}

template <int L>
AccKernel KernelTable<L>::konst_acc(Kind kk, bool affine) {
  return affine ? pick_konst_acc<L, true>(kk) : pick_konst_acc<L, false>(kk);
}

template <int L, bool Affine, bool Ind>
LoadAccKernel pick_load_acc(Kind k) {
  switch (k) {
    case Kind::Vec: return &k_load_acc<Kind::Vec, L, Affine, Ind>;
    case Kind::Lit: return &k_load_acc<Kind::Lit, L, Affine, Ind>;
    case Kind::Col: return &k_load_acc<Kind::Col, L, Affine, Ind>;
    case Kind::Gat: return &k_load_acc<Kind::Gat, L, Affine, Ind>;
  }
  return nullptr;
}

template <int L>
LoadAccKernel KernelTable<L>::load_acc(Kind k, bool affine, bool ind) {
  if (ind) return affine ? pick_load_acc<L, true, true>(k) : pick_load_acc<L, false, true>(k);
  return affine ? pick_load_acc<L, true, false>(k) : pick_load_acc<L, false, false>(k);
}

// ---- fused pair tables --------------------------------------------------------------------
//
// One enumeration serves the four sinks of a pair: the plain kernel in the contiguous (0) and
// indirect (1) row modes, and the Sum (2) / Affine (3) reduction epilogues. The shapes are those
// listed at KernelTable::pair; a commutative first op takes (S, G) and (G, G) only (the plan
// puts the scalar first), so the table holds 11 x 13 shapes per sink.

template <int L, int Sink>
struct PairSink {
  using Fn = std::conditional_t<(Sink < 2), OpKernel, AccKernel>;
  template <Op op, PK KA, PK KB, Op op2, PK KC, bool PR>
  static Fn get() {
    if constexpr (Sink == 0) return &k_pair<op, KA, KB, op2, KC, PR, L, false>;
    else if constexpr (Sink == 1) return &k_pair<op, KA, KB, op2, KC, PR, L, true>;
    else if constexpr (Sink == 2) return &k_pair_acc<op, KA, KB, op2, KC, PR, L, false>;
    else return &k_pair_acc<op, KA, KB, op2, KC, PR, L, true>;
  }
};

template <int L, int Sink, Op op, PK KA, PK KB, Op op2>
typename PairSink<L, Sink>::Fn pick_pair_3(PK kc, bool pr) {
  using PS = PairSink<L, Sink>;
  if constexpr (op2 == Op::Add || op2 == Op::Mul) {
    if (pr) return nullptr;  // commutative: the plan puts the pair's value on the left
    if (kc == PK::S) return PS::template get<op, KA, KB, op2, PK::S, false>();
    if (kc == PK::G) return PS::template get<op, KA, KB, op2, PK::G, false>();
    return nullptr;
  } else {
    if (kc == PK::S) return pr ? PS::template get<op, KA, KB, op2, PK::S, true>() : PS::template get<op, KA, KB, op2, PK::S, false>();
    if (kc == PK::G) return pr ? PS::template get<op, KA, KB, op2, PK::G, true>() : PS::template get<op, KA, KB, op2, PK::G, false>();
    return nullptr;
  }
}

template <int L, int Sink, Op op, PK KA, PK KB>
typename PairSink<L, Sink>::Fn pick_pair_2(Op op2, PK kc, bool pr) {
  switch (op2) {
    case Op::Add: return pick_pair_3<L, Sink, op, KA, KB, Op::Add>(kc, pr);
    case Op::Sub: return pick_pair_3<L, Sink, op, KA, KB, Op::Sub>(kc, pr);
    case Op::Mul: return pick_pair_3<L, Sink, op, KA, KB, Op::Mul>(kc, pr);
    case Op::Div: return pick_pair_3<L, Sink, op, KA, KB, Op::Div>(kc, pr);
    case Op::Neg: return kc == PK::None ? PairSink<L, Sink>::template get<op, KA, KB, Op::Neg, PK::None, false>() : nullptr;
    default: return nullptr;
  }
}

template <int L, int Sink, Op op>
typename PairSink<L, Sink>::Fn pick_pair_1(PK ka, PK kb, Op op2, PK kc, bool pr) {
  if constexpr (op == Op::Neg) {
    if (ka == PK::G && kb == PK::None) return pick_pair_2<L, Sink, Op::Neg, PK::G, PK::None>(op2, kc, pr);
    return nullptr;
  } else {
    if (ka == PK::S && kb == PK::G) return pick_pair_2<L, Sink, op, PK::S, PK::G>(op2, kc, pr);
    if (ka == PK::G && kb == PK::G) return pick_pair_2<L, Sink, op, PK::G, PK::G>(op2, kc, pr);
    if constexpr (op == Op::Sub || op == Op::Div) {
      if (ka == PK::G && kb == PK::S) return pick_pair_2<L, Sink, op, PK::G, PK::S>(op2, kc, pr);
    }
    return nullptr;
  }
}

template <int L, int Sink>
typename PairSink<L, Sink>::Fn pick_pair_0(Op op, PK ka, PK kb, Op op2, PK kc, bool pr) {
  switch (op) {
    case Op::Add: return pick_pair_1<L, Sink, Op::Add>(ka, kb, op2, kc, pr);
    case Op::Sub: return pick_pair_1<L, Sink, Op::Sub>(ka, kb, op2, kc, pr);
    case Op::Mul: return pick_pair_1<L, Sink, Op::Mul>(ka, kb, op2, kc, pr);
    case Op::Div: return pick_pair_1<L, Sink, Op::Div>(ka, kb, op2, kc, pr);
    case Op::Neg: return pick_pair_1<L, Sink, Op::Neg>(ka, kb, op2, kc, pr);
    default: return nullptr;
  }
}

template <int L>
OpKernel KernelTable<L>::pair(Op op, PK ka, PK kb, Op op2, PK kc, bool prev_right, bool ind) {
  return ind ? pick_pair_0<L, 1>(op, ka, kb, op2, kc, prev_right) : pick_pair_0<L, 0>(op, ka, kb, op2, kc, prev_right);
}

template <int L>
AccKernel KernelTable<L>::pair_acc(Op op, PK ka, PK kb, Op op2, PK kc, bool prev_right, bool affine) {
  return affine ? pick_pair_0<L, 3>(op, ka, kb, op2, kc, prev_right) : pick_pair_0<L, 2>(op, ka, kb, op2, kc, prev_right);
}

// ---- output copy -----------------------------------------------------------------------------

template <int L>
void KernelTable<L>::copy_out(const double* values, const std::int32_t* outputs, int n_out, double* out, int B, int b0,
                              int L_) {
  const std::size_t Bs = static_cast<std::size_t>(B);
  if constexpr (L > 0) {
    (void)L_;
    for (int o = 0; o < n_out; ++o) {
      const double* src = values + static_cast<std::size_t>(outputs[o]) * static_cast<std::size_t>(L);
      double* dst = out + static_cast<std::size_t>(o) * Bs + static_cast<std::size_t>(b0);
      double t[L];
      for (int l = 0; l < L; ++l) t[l] = src[l];
      for (int l = 0; l < L; ++l) dst[l] = t[l];
    }
  } else {
    const std::size_t LL = static_cast<std::size_t>(L_);
    for (int o = 0; o < n_out; ++o) {
      const double* src = values + static_cast<std::size_t>(outputs[o]) * LL;
      double* dst = out + static_cast<std::size_t>(o) * Bs + static_cast<std::size_t>(b0);
      for (int l = 0; l < L_; ++l) dst[l] = src[l];
    }
  }
}

}  // namespace epykos::exec::detail
