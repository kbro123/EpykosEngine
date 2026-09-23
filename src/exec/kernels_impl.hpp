// EpykosEngine — the interpreter's kernels, templated on op, operand kinds and lane width L
// (L = 0: runtime lane count from the context). Included by one TU per lane width
// (kernels_l*.cpp), which explicitly instantiates KernelTable<L>.
//
// Every kernel loops rows (i over the tile, r = r0 + i the domain row) and lanes (l < L) and
// performs exactly the IEEE operation the scalar evaluator performs, in the same order; the
// only freedom taken is which loop the compiler vectorises. Contraction is off in the including
// TUs (see kernels_l*.cpp), so an Affine fold's product and sum round separately.
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
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "epykos/hand/exp_poly.hpp"
#include "plan.hpp"

namespace epykos::exec::detail {

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

// ---- operand access ------------------------------------------------------------------------

// One element (L = 1 or the runtime variant's lane l): row r / tile row i / lane l of stride LL.
template <Kind K>
inline double elem(const double* values, const Operand& o, int r, std::size_t i, int l, std::size_t LL) noexcept {
  if constexpr (K == Kind::Vec) {
    (void)values; (void)r;
    return o.data[i * LL + static_cast<std::size_t>(l)];
  } else if constexpr (K == Kind::Lit) {
    (void)values; (void)r; (void)i; (void)l; (void)LL;
    return o.data[0];
  } else if constexpr (K == Kind::Col) {
    (void)values; (void)i; (void)l; (void)LL;
    return o.data[r];
  } else {
    (void)i;
    return values[static_cast<std::size_t>(o.index[r]) * LL + static_cast<std::size_t>(l)];
  }
}

// L > 1: stage row r's lanes of a vector-kind operand in `dst`; return the scalar of a scalar kind.
template <Kind K, int L>
inline double stage_row(double* dst, const double* values, const Operand& o, int r, std::size_t i) noexcept {
  if constexpr (K == Kind::Vec) {
    (void)values; (void)r;
    const double* src = o.data + i * static_cast<std::size_t>(L);
    for (int l = 0; l < L; ++l) dst[l] = src[l];
    return 0.0;
  } else if constexpr (K == Kind::Gat) {
    (void)i;
    const double* src = values + static_cast<std::size_t>(o.index[r]) * static_cast<std::size_t>(L);
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

// L = 1: stage rows [r0 + i, r0 + i + nb) of an operand in dst[0..nb).
template <Kind K>
inline void stage_rows1(double* dst, int nb, const double* values, const Operand& o, int r0, int i) noexcept {
  for (int j = 0; j < nb; ++j) dst[j] = elem<K>(values, o, r0 + i + j, static_cast<std::size_t>(i + j), 0, 1);
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

template <Op op, Kind KA, Kind KB, int L>
void k_binary(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* out) {
  const double* values = ctx.values;
  const Operand& a = s.a;
  const Operand& b = s.b;
  if constexpr (L == 1) {
    int i = 0;
    for (; i + row_block <= n; i += row_block) {
      double va[row_block], vb[row_block], o[row_block];
      stage_rows1<KA>(va, row_block, values, a, r0, i);
      stage_rows1<KB>(vb, row_block, values, b, r0, i);
      for (int j = 0; j < row_block; ++j) o[j] = apply2<op>(va[j], vb[j]);
      for (int j = 0; j < row_block; ++j) out[i + j] = o[j];
    }
    for (; i < n; ++i) {
      out[i] = apply2<op>(elem<KA>(values, a, r0 + i, static_cast<std::size_t>(i), 0, 1),
                          elem<KB>(values, b, r0 + i, static_cast<std::size_t>(i), 0, 1));
    }
  } else if constexpr (L > 1) {
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
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
      const int r = r0 + i;
      double* dst = out + static_cast<std::size_t>(i) * LL;
      for (int l = 0; l < ctx.L; ++l) {
        dst[l] = apply2<op>(elem<KA>(values, a, r, static_cast<std::size_t>(i), l, LL),
                            elem<KB>(values, b, r, static_cast<std::size_t>(i), l, LL));
      }
    }
  }
}

template <Op op, Kind KA, int L>
void k_unary(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* out) {
  const double* values = ctx.values;
  const Operand& a = s.a;
  if constexpr (L == 1) {
    int i = 0;
    for (; i + row_block <= n; i += row_block) {
      double va[row_block], o[row_block];
      stage_rows1<KA>(va, row_block, values, a, r0, i);
      for (int j = 0; j < row_block; ++j) o[j] = apply1<op>(va[j]);
      for (int j = 0; j < row_block; ++j) out[i + j] = o[j];
    }
    for (; i < n; ++i) out[i] = apply1<op>(elem<KA>(values, a, r0 + i, static_cast<std::size_t>(i), 0, 1));
  } else if constexpr (L > 1) {
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
      double va[L], o[L];
      const double sa = stage_row<KA, L>(va, values, a, r, static_cast<std::size_t>(i));
      for (int l = 0; l < L; ++l) o[l] = apply1<op>(lane<KA>(sa, va, l));
      double* dst = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) dst[l] = o[l];
    }
  } else {
    const std::size_t LL = static_cast<std::size_t>(ctx.L);
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
      double* dst = out + static_cast<std::size_t>(i) * LL;
      for (int l = 0; l < ctx.L; ++l) dst[l] = apply1<op>(elem<KA>(values, a, r, static_cast<std::size_t>(i), l, LL));
    }
  }
}

// Materialise an operand into a tile-shaped vector.
template <Kind K, int L>
void load_impl(const Operand& a, const RunCtx& ctx, int r0, int n, double* out) {
  const double* values = ctx.values;
  if constexpr (L == 1) {
    int i = 0;
    for (; i + row_block <= n; i += row_block) {
      double va[row_block];
      stage_rows1<K>(va, row_block, values, a, r0, i);
      for (int j = 0; j < row_block; ++j) out[i + j] = va[j];
    }
    for (; i < n; ++i) out[i] = elem<K>(values, a, r0 + i, static_cast<std::size_t>(i), 0, 1);
  } else if constexpr (L > 1) {
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
      double va[L];
      const double sa = stage_row<K, L>(va, values, a, r, static_cast<std::size_t>(i));
      double* dst = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(L);
      for (int l = 0; l < L; ++l) dst[l] = lane<K>(sa, va, l);
    }
  } else {
    const std::size_t LL = static_cast<std::size_t>(ctx.L);
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
      double* dst = out + static_cast<std::size_t>(i) * LL;
      for (int l = 0; l < ctx.L; ++l) dst[l] = elem<K>(values, a, r, static_cast<std::size_t>(i), l, LL);
    }
  }
}

template <Kind K, int L>
void k_load(const Operand& a, const RunCtx& ctx, int r0, int n, double* out) {
  load_impl<K, L>(a, ctx, r0, n, out);
}

// Exp through exp_poly: materialise the argument, then transform in place (E1).
template <Kind KA, int L>
void k_exp_poly(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* out) {
  load_impl<KA, L>(s.a, ctx, r0, n, out);
  hand::exp_poly_array(out, out, n * lanes<L>(ctx));
}

// Fma / Select over three materialised (Vec) operands: three scratch buffers, the output is a
// fourth; staged through locals so the loop vectorises without alias checks.
template <Op op, int L>
void k_ternary(const StepPlan& s, const RunCtx& ctx, int, int n, double* out) {
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
template <Kind KK, int L>
void k_konst(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* out) {
  load_impl<KK, L>(s.konst, ctx, r0, n, out);
}

// Input: values[(base + r)·L + l] = state[ordinal(r)·B + b0 + l].
template <int L>
void k_input(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* out) {
  const int LL = lanes<L>(ctx);
  const std::size_t LLs = static_cast<std::size_t>(LL);
  const double* state = ctx.state + ctx.b0;
  const std::size_t B = static_cast<std::size_t>(ctx.B);
  for (int i = 0; i < n; ++i) {
    const double* src = state + static_cast<std::size_t>(s.ordinal[r0 + i]) * B;
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
inline void seg_group(int i, int nj, int n, int len, const double* values, const std::int32_t* rows,
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
inline void seg_group1(int i, int nj, int n, int len, const double* values, const std::int32_t* rows,
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

// Whole-domain Sum / Affine over length buckets with transposed members (see plan.hpp).
template <bool Affine, int L>
void k_seg_whole(const SegPlan& sp, const RunCtx& ctx, double* dom) {
  const double* values = ctx.values;
  const double* konst = Affine ? sp.konst.data : nullptr;
  const bool konst_is_column = Affine && sp.konst.kind == Kind::Col;
  for (const SegBlock& blk : sp.blocks) {
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
      // Runtime lane count: the shared accumulator scratch, plain loops.
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
}

// Per-tile Sum / Affine, row by row (a Sum / Affine step that shares its group with other steps).
template <bool Affine, int L>
void k_seg_rows(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* out) {
  const int LL = lanes<L>(ctx);
  const std::size_t LLs = static_cast<std::size_t>(LL);
  const SegPlan& sp = *s.seg;
  const double* values = ctx.values;
  for (int i = 0; i < n; ++i) {
    const int r = r0 + i;
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

// ---- tables ----------------------------------------------------------------------------------

template <int L, Op op, Kind KA>
OpKernel pick_binary_b(Kind kb) {
  switch (kb) {
    case Kind::Vec: return &k_binary<op, KA, Kind::Vec, L>;
    case Kind::Lit: return &k_binary<op, KA, Kind::Lit, L>;
    case Kind::Col: return &k_binary<op, KA, Kind::Col, L>;
    case Kind::Gat: return &k_binary<op, KA, Kind::Gat, L>;
  }
  return nullptr;
}

template <int L, Op op>
OpKernel pick_binary_a(Kind ka, Kind kb) {
  switch (ka) {
    case Kind::Vec: return pick_binary_b<L, op, Kind::Vec>(kb);
    case Kind::Lit: return pick_binary_b<L, op, Kind::Lit>(kb);
    case Kind::Col: return pick_binary_b<L, op, Kind::Col>(kb);
    case Kind::Gat: return pick_binary_b<L, op, Kind::Gat>(kb);
  }
  return nullptr;
}

template <int L>
OpKernel KernelTable<L>::binary(Op op, Kind ka, Kind kb) {
  switch (op) {
    case Op::Add: return pick_binary_a<L, Op::Add>(ka, kb);
    case Op::Sub: return pick_binary_a<L, Op::Sub>(ka, kb);
    case Op::Mul: return pick_binary_a<L, Op::Mul>(ka, kb);
    case Op::Div: return pick_binary_a<L, Op::Div>(ka, kb);
    case Op::CmpLt: return pick_binary_a<L, Op::CmpLt>(ka, kb);
    case Op::CmpLe: return pick_binary_a<L, Op::CmpLe>(ka, kb);
    case Op::CmpGt: return pick_binary_a<L, Op::CmpGt>(ka, kb);
    case Op::CmpGe: return pick_binary_a<L, Op::CmpGe>(ka, kb);
    case Op::CmpEq: return pick_binary_a<L, Op::CmpEq>(ka, kb);
    default: throw std::invalid_argument("exec: not a binary op");
  }
}

template <int L, Op op>
OpKernel pick_unary_a(Kind ka) {
  switch (ka) {
    case Kind::Vec: return &k_unary<op, Kind::Vec, L>;
    case Kind::Lit: return &k_unary<op, Kind::Lit, L>;
    case Kind::Col: return &k_unary<op, Kind::Col, L>;
    case Kind::Gat: return &k_unary<op, Kind::Gat, L>;
  }
  return nullptr;
}

template <int L>
OpKernel KernelTable<L>::unary(Op op, Kind ka) {
  switch (op) {
    case Op::Neg: return pick_unary_a<L, Op::Neg>(ka);
    case Op::Exp: return pick_unary_a<L, Op::Exp>(ka);
    case Op::Log: return pick_unary_a<L, Op::Log>(ka);
    case Op::Sqrt: return pick_unary_a<L, Op::Sqrt>(ka);
    case Op::Recip: return pick_unary_a<L, Op::Recip>(ka);
    default: throw std::invalid_argument("exec: not a unary op");
  }
}

template <int L>
OpKernel KernelTable<L>::exp_poly(Kind ka) {
  switch (ka) {
    case Kind::Vec: return &k_exp_poly<Kind::Vec, L>;
    case Kind::Lit: return &k_exp_poly<Kind::Lit, L>;
    case Kind::Col: return &k_exp_poly<Kind::Col, L>;
    case Kind::Gat: return &k_exp_poly<Kind::Gat, L>;
  }
  return nullptr;
}

template <int L>
OpKernel KernelTable<L>::ternary(Op op) {
  switch (op) {
    case Op::Fma: return &k_ternary<Op::Fma, L>;
    case Op::Select: return &k_ternary<Op::Select, L>;
    default: throw std::invalid_argument("exec: not a ternary op");
  }
}

template <int L>
OpKernel KernelTable<L>::konst(Kind kk) {
  switch (kk) {
    case Kind::Lit: return &k_konst<Kind::Lit, L>;
    case Kind::Col: return &k_konst<Kind::Col, L>;
    default: throw std::invalid_argument("exec: a Const step needs a literal or a column");
  }
}

template <int L>
OpKernel KernelTable<L>::input() {
  return &k_input<L>;
}

template <int L>
OpKernel KernelTable<L>::seg_rows(bool affine) {
  return affine ? &k_seg_rows<true, L> : &k_seg_rows<false, L>;
}

template <int L>
LoadKernel KernelTable<L>::load(Kind k) {
  switch (k) {
    case Kind::Vec: return &k_load<Kind::Vec, L>;
    case Kind::Lit: return &k_load<Kind::Lit, L>;
    case Kind::Col: return &k_load<Kind::Col, L>;
    case Kind::Gat: return &k_load<Kind::Gat, L>;
  }
  return nullptr;
}

template <int L>
SegKernel KernelTable<L>::seg_whole(bool affine) {
  return affine ? &k_seg_whole<true, L> : &k_seg_whole<false, L>;
}

}  // namespace epykos::exec::detail
