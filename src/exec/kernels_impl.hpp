// EpykosEngine — the interpreter's kernels, templated on op, operand kinds and lane width L
// (L = 0: runtime lane count from the context). Included by one TU per lane width
// (kernels_l*.cpp), which explicitly instantiates KernelTable<L>.
//
// Every kernel loops rows (i over the tile, r = r0 + i the domain row) and lanes (l < L) and
// performs exactly the IEEE operation the scalar evaluator performs, in the same order; the
// only freedom taken is which loop the compiler vectorises. Contraction is off in the including
// TUs (see kernels_l*.cpp), so an Affine fold's product and sum round separately.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "epykos/hand/exp_poly.hpp"
#include "plan.hpp"

namespace epykos::exec::detail {

template <int L>
inline int lanes(const RunCtx& ctx) noexcept {
  if constexpr (L > 0) {
    (void)ctx;
    return L;
  } else {
    return ctx.L;
  }
}

template <Kind K>
inline double fetch(const Operand& o, const double* values, int r, std::size_t i, int l, int LL) noexcept {
  if constexpr (K == Kind::Vec) {
    (void)values; (void)r;
    return o.data[i * static_cast<std::size_t>(LL) + static_cast<std::size_t>(l)];
  } else if constexpr (K == Kind::Lit) {
    (void)values; (void)r; (void)i; (void)l; (void)LL;
    return o.data[0];
  } else if constexpr (K == Kind::Col) {
    (void)values; (void)i; (void)l; (void)LL;
    return o.data[r];
  } else {
    (void)i;
    return values[static_cast<std::size_t>(o.index[r]) * static_cast<std::size_t>(LL) + static_cast<std::size_t>(l)];
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

template <Op op, Kind KA, Kind KB, int L>
void k_binary(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* __restrict out) {
  const int LL = lanes<L>(ctx);
  const Operand& a = s.a;
  const Operand& b = s.b;
  const double* values = ctx.values;
  if constexpr (L == 1) {
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
      out[i] = apply2<op>(fetch<KA>(a, values, r, static_cast<std::size_t>(i), 0, 1),
                          fetch<KB>(b, values, r, static_cast<std::size_t>(i), 0, 1));
    }
  } else {
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
      double* o = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(LL);
      for (int l = 0; l < LL; ++l) {
        o[l] = apply2<op>(fetch<KA>(a, values, r, static_cast<std::size_t>(i), l, LL),
                          fetch<KB>(b, values, r, static_cast<std::size_t>(i), l, LL));
      }
    }
  }
}

template <Op op, Kind KA, int L>
void k_unary(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* __restrict out) {
  const int LL = lanes<L>(ctx);
  const Operand& a = s.a;
  const double* values = ctx.values;
  if constexpr (L == 1) {
    for (int i = 0; i < n; ++i) {
      out[i] = apply1<op>(fetch<KA>(a, values, r0 + i, static_cast<std::size_t>(i), 0, 1));
    }
  } else {
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
      double* o = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(LL);
      for (int l = 0; l < LL; ++l) o[l] = apply1<op>(fetch<KA>(a, values, r, static_cast<std::size_t>(i), l, LL));
    }
  }
}

// Materialise an operand into a tile-shaped vector.
template <Kind K, int L>
void k_load(const Operand& a, const RunCtx& ctx, int r0, int n, double* __restrict out) {
  const int LL = lanes<L>(ctx);
  const double* values = ctx.values;
  if constexpr (L == 1) {
    for (int i = 0; i < n; ++i) out[i] = fetch<K>(a, values, r0 + i, static_cast<std::size_t>(i), 0, 1);
  } else {
    for (int i = 0; i < n; ++i) {
      const int r = r0 + i;
      double* o = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(LL);
      for (int l = 0; l < LL; ++l) o[l] = fetch<K>(a, values, r, static_cast<std::size_t>(i), l, LL);
    }
  }
}

// Exp through exp_poly: materialise the argument, then transform in place (E1).
template <Kind KA, int L>
void k_exp_poly(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* __restrict out) {
  k_load<KA, L>(s.a, ctx, r0, n, out);
  hand::exp_poly_array(out, out, n * lanes<L>(ctx));
}

// Fma / Select over three materialised (Vec) operands.
template <Op op, int L>
void k_ternary(const StepPlan& s, const RunCtx& ctx, int, int n, double* __restrict out) {
  const int LL = lanes<L>(ctx);
  const double* a = s.a.data;
  const double* b = s.b.data;
  const double* c = s.c.data;
  const std::size_t total = static_cast<std::size_t>(n) * static_cast<std::size_t>(LL);
  for (std::size_t e = 0; e < total; ++e) out[e] = apply3<op>(a[e], b[e], c[e]);
}

// Const: the row's constant (literal or column) broadcast over the lanes.
template <Kind KK, int L>
void k_konst(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* __restrict out) {
  k_load<KK, L>(s.konst, ctx, r0, n, out);
}

// Input: values[(base + r)·L + l] = state[ordinal(r)·B + b0 + l].
template <int L>
void k_input(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* __restrict out) {
  const int LL = lanes<L>(ctx);
  const double* state = ctx.state + ctx.b0;
  const std::size_t B = static_cast<std::size_t>(ctx.B);
  for (int i = 0; i < n; ++i) {
    const double* src = state + static_cast<std::size_t>(s.ordinal[r0 + i]) * B;
    double* o = out + static_cast<std::size_t>(i) * static_cast<std::size_t>(LL);
    for (int l = 0; l < LL; ++l) o[l] = src[l];
  }
}

// ---- segments ------------------------------------------------------------------------------

// Whole-domain Sum / Affine over length buckets with transposed members. For each bucket: the
// accumulator of row i starts at member 0 (Sum) or c_0 (Affine); fold step k adds member k of
// every row of the bucket (one vector op over rows × lanes, each row's own left fold order);
// then the rows are scattered to their places in the domain.
template <bool Affine, int L>
void k_seg_whole(const SegPlan& sp, const RunCtx& ctx, double* __restrict dom) {
  const int LL = lanes<L>(ctx);
  const std::size_t LLs = static_cast<std::size_t>(LL);
  double* __restrict acc = ctx.acc;
  const double* values = ctx.values;
  for (const SegBlock& blk : sp.blocks) {
    const int n = blk.n;
    const int len = blk.len;
    const std::int32_t* rows = sp.rows.data() + blk.row0;
    const std::int32_t* memT = sp.memT.data() + blk.memT;
    const double* coefT = Affine ? sp.coefT.data() + blk.memT : nullptr;
    int k0;
    if constexpr (Affine) {
      const Operand& c0 = sp.konst;
      if (c0.kind == Kind::Col) {
        for (int i = 0; i < n; ++i) {
          const double v = c0.data[rows[i]];
          for (int l = 0; l < LL; ++l) acc[static_cast<std::size_t>(i) * LLs + static_cast<std::size_t>(l)] = v;
        }
      } else {
        const double v = c0.data[0];
        for (std::size_t e = 0; e < static_cast<std::size_t>(n) * LLs; ++e) acc[e] = v;
      }
      k0 = 0;
    } else {
      if constexpr (L == 1) {
        for (int i = 0; i < n; ++i) acc[i] = values[memT[i]];
      } else {
        for (int i = 0; i < n; ++i) {
          const double* src = values + static_cast<std::size_t>(memT[i]) * LLs;
          double* d = acc + static_cast<std::size_t>(i) * LLs;
          for (int l = 0; l < LL; ++l) d[l] = src[l];
        }
      }
      k0 = 1;
    }
    for (int k = k0; k < len; ++k) {
      const std::int32_t* mk = memT + static_cast<std::size_t>(k) * static_cast<std::size_t>(n);
      if constexpr (Affine) {
        const double* ck = coefT + static_cast<std::size_t>(k) * static_cast<std::size_t>(n);
        if constexpr (L == 1) {
          for (int i = 0; i < n; ++i) {
            const double p = ck[i] * values[mk[i]];
            acc[i] = acc[i] + p;
          }
        } else {
          for (int i = 0; i < n; ++i) {
            const double* src = values + static_cast<std::size_t>(mk[i]) * LLs;
            double* d = acc + static_cast<std::size_t>(i) * LLs;
            const double c = ck[i];
            for (int l = 0; l < LL; ++l) {
              const double p = c * src[l];
              d[l] = d[l] + p;
            }
          }
        }
      } else {
        if constexpr (L == 1) {
          for (int i = 0; i < n; ++i) acc[i] = acc[i] + values[mk[i]];
        } else {
          for (int i = 0; i < n; ++i) {
            const double* src = values + static_cast<std::size_t>(mk[i]) * LLs;
            double* d = acc + static_cast<std::size_t>(i) * LLs;
            for (int l = 0; l < LL; ++l) d[l] = d[l] + src[l];
          }
        }
      }
    }
    if constexpr (L == 1) {
      for (int i = 0; i < n; ++i) dom[rows[i]] = acc[i];
    } else {
      for (int i = 0; i < n; ++i) {
        double* d = dom + static_cast<std::size_t>(rows[i]) * LLs;
        const double* src = acc + static_cast<std::size_t>(i) * LLs;
        for (int l = 0; l < LL; ++l) d[l] = src[l];
      }
    }
  }
}

// Per-tile Sum / Affine, row by row (a Sum / Affine step that shares its group with other steps).
template <bool Affine, int L>
void k_seg_rows(const StepPlan& s, const RunCtx& ctx, int r0, int n, double* __restrict out) {
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
