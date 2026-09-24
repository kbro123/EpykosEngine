// EpykosEngine — the adjoint runtime (include/epykos/adjoint/adjoint.hpp): the forward pass
// over the value buffer and the reverse pass of the plan, per lane chunk, per tile of rows.
//
// An E0 TU (src/**/*_e0.cpp, root CMakeLists.txt, D25): compiled with -ffp-contract=off in every
// preset on every compiler, so the forward is bit-identical to tape/replay.hpp and the
// interpreter, and the reverse's products and sums round separately, whatever the compiler's
// contraction default. Every loop is per (row, lane): a lane never reads another lane and a row
// never reads another row of the tile, so the bits do not depend on B, tile or lane_tile.
#ifndef EPYKOS_FP_CONTRACT_OFF
#error "adjoint_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in CMakeLists.txt)"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef EPYKOS_EXEC_PROFILE
#include <chrono>
#endif

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/catalogue/kernel.hpp"
#include "epykos/catalogue/registry.hpp"
#include "epykos/catalogue/signature.hpp"
#include "epykos/mutation/mutation.hpp"

namespace epykos::adjoint {

namespace {

using std::size_t;
using ir::Domain;
using ir::Group;
using ir::Program;
using ir::Segment;
using ir::Slot;
using ir::SlotKind;
using ir::Step;

inline size_t idx(std::int32_t i) { return static_cast<size_t>(i); }

// Everything a chunk's passes read: the program, the plan, the buffers and the run's arrays.
struct Ctx {
  const Program* p = nullptr;
  const AdjointPlan* plan = nullptr;
  double* values = nullptr;   // [v·L + l]
  double* vbar = nullptr;     // [v·L + l]
  double* gbar = nullptr;     // [gather slot·L + l]
  double* segbar = nullptr;   // [segment slot·L + l]
  double* fs = nullptr;       // step values of the tile: [k·tile_elems + i·L + l]
  double* sbar = nullptr;     // step adjoints of the tile: same layout
  double* ops = nullptr;      // loaded operands: [(3k + which)·tile_elems + i·L + l]
  size_t tile_elems = 0;
  int tile = 0;
  const double* state = nullptr;
  const double* out_bar = nullptr;
  double* out = nullptr;
  double* state_bar = nullptr;
  int B = 0;
  int b0 = 0;
  int L = 0;
  // Mutants (D32; M2/Q4b), queried once per run() and read by the rules only in the mutation
  // build (mutation::compiled_in is a constexpr false elsewhere, so the checks fold away).
  bool select_wrong_arm = false;  // adjoint.select_wrong_arm: the adjoint goes to the other arm
  bool recip_rule_sign = false;   // adjoint.recip_rule_sign: abar += (ybar*y)*y instead of -=
  bool scan_forward_order = false;  // adjoint.scan_forward_order: the reverse scan runs forwards
  // M4/C1: per domain, the catalogued kernel for its signature (registry.hpp), or nullptr — both
  // arrays sized p->domains.size(), owned by Impl, built once at construction (nullptr
  // everywhere when Options::use_catalogue is false). forward() below calls the kernel directly
  // over the WHOLE domain instead of this file's own generic per-step loop.
  const catalogue::Kernel* cat_kernels = nullptr;
  const catalogue::DomainBinding* cat_bindings = nullptr;
#ifdef EPYKOS_EXEC_PROFILE
  double* cat_ns = nullptr;    // Impl-owned accumulators (this instance only)
  double* total_ns = nullptr;
#endif
};

// ---- flat elementwise kernels over N = n·L contiguous elements (row-major, lane innermost).
// Forward: y = op(a, b, c). Reverse: the local rules of plan.hpp, `+=` into the targets.
#define EPY_RESTRICT __restrict

inline void fwd_add(size_t N, const double* EPY_RESTRICT a, const double* EPY_RESTRICT b, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = a[e] + b[e];
}
inline void fwd_sub(size_t N, const double* EPY_RESTRICT a, const double* EPY_RESTRICT b, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = a[e] - b[e];
}
inline void fwd_mul(size_t N, const double* EPY_RESTRICT a, const double* EPY_RESTRICT b, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = a[e] * b[e];
}
inline void fwd_div(size_t N, const double* EPY_RESTRICT a, const double* EPY_RESTRICT b, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = a[e] / b[e];
}
inline void fwd_neg(size_t N, const double* EPY_RESTRICT a, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = -a[e];
}
inline void fwd_exp(size_t N, const double* EPY_RESTRICT a, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = std::exp(a[e]);
}
inline void fwd_log(size_t N, const double* EPY_RESTRICT a, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = std::log(a[e]);
}
inline void fwd_sqrt(size_t N, const double* EPY_RESTRICT a, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = std::sqrt(a[e]);
}
inline void fwd_recip(size_t N, const double* EPY_RESTRICT a, double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = 1.0 / a[e];
}
inline void fwd_fma(size_t N, const double* EPY_RESTRICT a, const double* EPY_RESTRICT b, const double* EPY_RESTRICT c,
                    double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = std::fma(a[e], b[e], c[e]);
}
inline void fwd_select(size_t N, const double* EPY_RESTRICT a, const double* EPY_RESTRICT b, const double* EPY_RESTRICT c,
                       double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) y[e] = (a[e] != 0.0) ? b[e] : c[e];
}
template <class Cmp>
inline void fwd_cmp(size_t N, const double* EPY_RESTRICT a, const double* EPY_RESTRICT b, double* EPY_RESTRICT y, Cmp cmp) {
  for (size_t e = 0; e < N; ++e) y[e] = cmp(a[e], b[e]) ? 1.0 : 0.0;
}

// t += yb  /  t -= yb  /  t += yb·m  /  t -= (yb/b)·y etc. Targets may be null (no adjoint).
inline void acc_plus(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb) {
  for (size_t e = 0; e < N; ++e) t[e] += yb[e];
}
inline void acc_minus(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb) {
  for (size_t e = 0; e < N; ++e) t[e] -= yb[e];
}
inline void acc_plus_mul(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb, const double* EPY_RESTRICT m) {
  for (size_t e = 0; e < N; ++e) t[e] += yb[e] * m[e];
}
inline void acc_plus_div(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb, const double* EPY_RESTRICT m) {
  for (size_t e = 0; e < N; ++e) t[e] += yb[e] / m[e];
}
inline void acc_exp(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb, const double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) t[e] += yb[e] * y[e];
}
inline void acc_sqrt(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb, const double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) t[e] += (0.5 * yb[e]) / y[e];
}
inline void acc_recip(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb, const double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) t[e] -= (yb[e] * y[e]) * y[e];
}
// Mutant adjoint.recip_rule_sign: the sign of the reciprocal's rule.
inline void acc_recip_wrong_sign(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb, const double* EPY_RESTRICT y) {
  for (size_t e = 0; e < N; ++e) t[e] += (yb[e] * y[e]) * y[e];
}
// Div: abar += ybar/b; bbar -= (ybar/b)·y — the quotient t is computed once per element.
inline void acc_div(size_t N, double* EPY_RESTRICT ta, double* EPY_RESTRICT tb, const double* EPY_RESTRICT yb,
                    const double* EPY_RESTRICT b, const double* EPY_RESTRICT y) {
  if (ta != nullptr && tb != nullptr) {
    for (size_t e = 0; e < N; ++e) {
      const double t = yb[e] / b[e];
      ta[e] += t;
      tb[e] -= t * y[e];
    }
  } else if (ta != nullptr) {
    for (size_t e = 0; e < N; ++e) ta[e] += yb[e] / b[e];
  } else if (tb != nullptr) {
    for (size_t e = 0; e < N; ++e) {
      const double t = yb[e] / b[e];
      tb[e] -= t * y[e];
    }
  }
}
inline void acc_select_true(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb, const double* EPY_RESTRICT a) {
  for (size_t e = 0; e < N; ++e) t[e] += (a[e] != 0.0) ? yb[e] : 0.0;
}
inline void acc_select_false(size_t N, double* EPY_RESTRICT t, const double* EPY_RESTRICT yb, const double* EPY_RESTRICT a) {
  for (size_t e = 0; e < N; ++e) t[e] += (a[e] != 0.0) ? 0.0 : yb[e];
}

// ---- the chunk passes for a lane width known at compile time (LT > 0) or at run time (LT = 0).
template <int LT>
struct Lanes {
  static int lanes(const Ctx& c) noexcept { return LT > 0 ? LT : c.L; }

  // Loads a literal / column / gather operand of tile rows r0 .. r0 + n into dst[i·L + l].
  static void load_into(const Ctx& c, double* EPY_RESTRICT dst, const Slot& sl, int r0, int n) {
    const int L = lanes(c);
    const Program& p = *c.p;
    switch (sl.kind) {
      case SlotKind::Literal: {
        const double v = p.literals[idx(sl.index)];
        const size_t N = static_cast<size_t>(n) * static_cast<size_t>(L);
        for (size_t e = 0; e < N; ++e) dst[e] = v;
        break;
      }
      case SlotKind::Column: {
        const double* col = p.columns[idx(sl.index)].values.data() + r0;
        for (int i = 0; i < n; ++i) {
          const double v = col[i];
          double* d = dst + static_cast<size_t>(i) * static_cast<size_t>(L);
          for (int l = 0; l < L; ++l) d[l] = v;
        }
        break;
      }
      case SlotKind::Gather: {
        const ir::value_id* index = p.gathers[idx(sl.index)].index.data() + r0;
        for (int i = 0; i < n; ++i) {
          const double* src = c.values + idx(index[i]) * static_cast<size_t>(L);
          double* d = dst + static_cast<size_t>(i) * static_cast<size_t>(L);
          for (int l = 0; l < L; ++l) d[l] = src[l];
        }
        break;
      }
      default:
        break;
    }
  }

  // The operand's tile vector: an earlier step's values, or the loaded copy (loading it when
  // `load`). Null for None / Segment / Input slots.
  static const double* operand(const Ctx& c, const Slot& sl, int k, int which, int r0, int n, bool load) {
    switch (sl.kind) {
      case SlotKind::Step: return c.fs + idx(sl.index) * c.tile_elems;
      case SlotKind::Literal:
      case SlotKind::Column:
      case SlotKind::Gather: {
        double* dst = c.ops + (3 * static_cast<size_t>(k) + static_cast<size_t>(which)) * c.tile_elems;
        if (load) load_into(c, dst, sl, r0, n);
        return dst;
      }
      default: return nullptr;
    }
  }

  // Where an operand's adjoint accumulates: an earlier step's adjoint or the gather's edge
  // slots for these rows; null when the operand receives none (literal, column, none).
  static double* target(const Ctx& c, const Slot& sl, int r0) {
    switch (sl.kind) {
      case SlotKind::Step: return c.sbar + idx(sl.index) * c.tile_elems;
      case SlotKind::Gather:
        return c.gbar + (idx(c.plan->gather_slot_base[idx(sl.index)]) + static_cast<size_t>(r0)) * static_cast<size_t>(lanes(c));
      default: return nullptr;
    }
  }

  // Forward of step k of domain d for tile rows r0 .. r0 + n into y[i·L + l] (operands loaded).
  static void forward_step(const Ctx& c, int d, const Step& s, int k, int r0, int n, double* y) {
    const int L = lanes(c);
    const Program& p = *c.p;
    const size_t N = static_cast<size_t>(n) * static_cast<size_t>(L);
    switch (s.op) {
      case Op::Const: load_into(c, y, s.konst, r0, n); break;
      case Op::Input: {
        const std::int32_t* ord = c.plan->domains[idx(d)].ordinal.data() + r0;
        for (int i = 0; i < n; ++i) {
          const double* src = c.state + idx(ord[i]) * static_cast<size_t>(c.B) + static_cast<size_t>(c.b0);
          double* dst = y + static_cast<size_t>(i) * static_cast<size_t>(L);
          for (int l = 0; l < L; ++l) dst[l] = src[l];
        }
        break;
      }
      case Op::Sum: {
        if (ir::is_fixed_sum(s)) {
          // A scan group's Sum over operand slots: the left fold of the loaded operands.
          const double* a = operand(c, s.a, k, 0, r0, n, true);
          const double* b = operand(c, s.b, k, 1, r0, n, true);
          const double* cc = operand(c, s.c, k, 2, r0, n, true);
          for (size_t e = 0; e < N; ++e) y[e] = a[e] + b[e];
          if (cc != nullptr) {
            for (size_t e = 0; e < N; ++e) y[e] = y[e] + cc[e];
          }
          break;
        }
        const Segment& seg = p.segments[idx(s.a.index)];
        for (int i = 0; i < n; ++i) {
          const std::int32_t lo = seg.offsets[idx(r0 + i)];
          const std::int32_t hi = seg.offsets[idx(r0 + i) + 1];
          double* dst = y + static_cast<size_t>(i) * static_cast<size_t>(L);
          const double* m0 = c.values + idx(seg.members[idx(lo)]) * static_cast<size_t>(L);
          for (int l = 0; l < L; ++l) dst[l] = m0[l];
          for (std::int32_t m = lo + 1; m < hi; ++m) {
            const double* mv = c.values + idx(seg.members[idx(m)]) * static_cast<size_t>(L);
            for (int l = 0; l < L; ++l) dst[l] = dst[l] + mv[l];
          }
        }
        break;
      }
      case Op::Affine: {
        const Segment& seg = p.segments[idx(s.a.index)];
        const double* c0 = operand(c, s.konst, k, 0, r0, n, true);
        for (int i = 0; i < n; ++i) {
          const std::int32_t lo = seg.offsets[idx(r0 + i)];
          const std::int32_t hi = seg.offsets[idx(r0 + i) + 1];
          double* dst = y + static_cast<size_t>(i) * static_cast<size_t>(L);
          const double* k0 = c0 + static_cast<size_t>(i) * static_cast<size_t>(L);
          for (int l = 0; l < L; ++l) dst[l] = k0[l];
          for (std::int32_t m = lo; m < hi; ++m) {
            const double coef = seg.coefs[idx(m)];
            const double* mv = c.values + idx(seg.members[idx(m)]) * static_cast<size_t>(L);
            for (int l = 0; l < L; ++l) {
              const double prod = coef * mv[l];
              dst[l] = dst[l] + prod;
            }
          }
        }
        break;
      }
      default: {
        const double* a = operand(c, s.a, k, 0, r0, n, true);
        const double* b = operand(c, s.b, k, 1, r0, n, true);
        const double* cc = operand(c, s.c, k, 2, r0, n, true);
        switch (s.op) {
          case Op::Add: fwd_add(N, a, b, y); break;
          case Op::Sub: fwd_sub(N, a, b, y); break;
          case Op::Mul: fwd_mul(N, a, b, y); break;
          case Op::Div: fwd_div(N, a, b, y); break;
          case Op::Neg: fwd_neg(N, a, y); break;
          case Op::Exp: fwd_exp(N, a, y); break;
          case Op::Log: fwd_log(N, a, y); break;
          case Op::Sqrt: fwd_sqrt(N, a, y); break;
          case Op::Recip: fwd_recip(N, a, y); break;
          case Op::Fma: fwd_fma(N, a, b, cc, y); break;
          case Op::Select: fwd_select(N, a, b, cc, y); break;
          case Op::CmpLt: fwd_cmp(N, a, b, y, [](double x, double z) { return x < z; }); break;
          case Op::CmpLe: fwd_cmp(N, a, b, y, [](double x, double z) { return x <= z; }); break;
          case Op::CmpGt: fwd_cmp(N, a, b, y, [](double x, double z) { return x > z; }); break;
          case Op::CmpGe: fwd_cmp(N, a, b, y, [](double x, double z) { return x >= z; }); break;
          case Op::CmpEq: fwd_cmp(N, a, b, y, [](double x, double z) { return x == z; }); break;
          default: break;  // validated away
        }
        break;
      }
    }
  }

  // Reverse of step k (operands already loaded by the recompute or by the caller): yb is the
  // step's adjoint, y its value.
  static void reverse_step(const Ctx& c, int d, const Step& s, int k, int r0, int n, const double* yb, const double* y) {
    const int L = lanes(c);
    const size_t N = static_cast<size_t>(n) * static_cast<size_t>(L);
    switch (s.op) {
      case Op::Const:
      case Op::CmpLt:
      case Op::CmpLe:
      case Op::CmpGt:
      case Op::CmpGe:
      case Op::CmpEq:
        break;
      case Op::Input: {
        const std::int32_t* ord = c.plan->domains[idx(d)].ordinal.data() + r0;
        for (int i = 0; i < n; ++i) {
          double* dst = c.state_bar + idx(ord[i]) * static_cast<size_t>(c.B) + static_cast<size_t>(c.b0);
          const double* src = yb + static_cast<size_t>(i) * static_cast<size_t>(L);
          for (int l = 0; l < L; ++l) dst[l] = src[l];
        }
        break;
      }
      case Op::Sum:
      case Op::Affine: {
        if (ir::is_fixed_sum(s)) {
          // Every member of the fold receives the adjoint (a, then b, then c).
          double* ta = target(c, s.a, r0);
          double* tb = target(c, s.b, r0);
          double* tc = target(c, s.c, r0);
          if (ta) acc_plus(N, ta, yb);
          if (tb) acc_plus(N, tb, yb);
          if (tc) acc_plus(N, tc, yb);
          break;
        }
        double* t = c.segbar + (idx(c.plan->segment_slot_base[idx(s.a.index)]) + static_cast<size_t>(r0)) * static_cast<size_t>(L);
        acc_plus(N, t, yb);
        break;
      }
      default: {
        const double* a = operand(c, s.a, k, 0, r0, n, false);
        const double* b = operand(c, s.b, k, 1, r0, n, false);
        double* ta = target(c, s.a, r0);
        double* tb = target(c, s.b, r0);
        double* tc = target(c, s.c, r0);
        switch (s.op) {
          case Op::Add:
            if (ta) acc_plus(N, ta, yb);
            if (tb) acc_plus(N, tb, yb);
            break;
          case Op::Sub:
            if (ta) acc_plus(N, ta, yb);
            if (tb) acc_minus(N, tb, yb);
            break;
          case Op::Mul:
            if (ta) acc_plus_mul(N, ta, yb, b);
            if (tb) acc_plus_mul(N, tb, yb, a);
            break;
          case Op::Div: acc_div(N, ta, tb, yb, b, y); break;
          case Op::Neg:
            if (ta) acc_minus(N, ta, yb);
            break;
          case Op::Exp:
            if (ta) acc_exp(N, ta, yb, y);
            break;
          case Op::Log:
            if (ta) acc_plus_div(N, ta, yb, a);
            break;
          case Op::Sqrt:
            if (ta) acc_sqrt(N, ta, yb, y);
            break;
          case Op::Recip:
            if (ta) {
              if (mutation::compiled_in && c.recip_rule_sign) acc_recip_wrong_sign(N, ta, yb, y);
              else acc_recip(N, ta, yb, y);
            }
            break;
          case Op::Fma:
            if (ta) acc_plus_mul(N, ta, yb, b);
            if (tb) acc_plus_mul(N, tb, yb, a);
            if (tc) acc_plus(N, tc, yb);
            break;
          case Op::Select:
            if (mutation::compiled_in && c.select_wrong_arm) {
              if (tb) acc_select_false(N, tb, yb, a);
              if (tc) acc_select_true(N, tc, yb, a);
            } else {
              if (tb) acc_select_true(N, tb, yb, a);
              if (tc) acc_select_false(N, tc, yb, a);
            }
            break;
          default: break;
        }
        break;
      }
    }
  }

  // The forward pass of the chunk: every domain in order, tiles of rows; the last step of a
  // group is written straight into the value buffer.
  static void forward(const Ctx& c) {
    const Program& p = *c.p;
    const int L = lanes(c);
    for (size_t d = 0; d < p.domains.size(); ++d) {
      const Domain& dom = p.domains[d];
      const Group& g = p.groups[d];
      const int last = static_cast<int>(g.steps.size()) - 1;
      double* dom_values = c.values + idx(dom.value_base) * static_cast<size_t>(L);
#ifdef EPYKOS_EXEC_PROFILE
      const auto t0 = std::chrono::steady_clock::now();
#endif
      // M4/C1 (PROBLEM.md §7; DESIGN.md §7's "the reverse of a fused group is rewrite /
      // catalogue work (M4)"): this file applies none of exec::Interpreter's fuse/inline
      // optimisations -- every domain's rows are always materialised here, unconditionally --
      // so a catalogued kernel is a safe, unconditional replacement for ANY catalogue-eligible
      // domain, scan included: it writes rows in ascending order into the SAME shared `values`
      // this loop would have, so a scan's carry (an ordinary Gather into this same domain's
      // earlier rows) resolves exactly as it would one row at a time.
      if (c.cat_kernels != nullptr && c.cat_kernels[d] != nullptr) {
        c.cat_bindings[d].call(c.cat_kernels[d], c.values, dom.value_base, 0, dom.rows, L);
#ifdef EPYKOS_EXEC_PROFILE
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        *c.total_ns += ns;
        *c.cat_ns += ns;
#endif
        continue;
      }
      // A scan's rows read earlier rows of the domain through the carry: one row per tile, in
      // row order, so that every operand load sees the previous row's value (D41).
      const int tile = c.plan->domains[d].is_scan ? 1 : c.tile;
      for (int r0 = 0; r0 < dom.rows; r0 += tile) {
        const int n = std::min(tile, dom.rows - r0);
        for (int k = 0; k <= last; ++k) {
          double* y = k == last ? dom_values + static_cast<size_t>(r0) * static_cast<size_t>(L)
                                : c.fs + static_cast<size_t>(k) * c.tile_elems;
          forward_step(c, static_cast<int>(d), g.steps[idx(k)], k, r0, n, y);
        }
      }
#ifdef EPYKOS_EXEC_PROFILE
      *c.total_ns += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
#endif
    }
    if (c.out != nullptr) {
      for (size_t o = 0; o < p.outputs.size(); ++o) {
        const double* src = c.values + idx(p.outputs[o]) * static_cast<size_t>(L);
        double* dst = c.out + o * static_cast<size_t>(c.B) + static_cast<size_t>(c.b0);
        for (int l = 0; l < L; ++l) dst[l] = src[l];
      }
    }
  }

  // The pull of rows r0 .. r0 + n of domain d: v̄ from the readers' edge slots (plan.hpp order).
  static void pull(const Ctx& c, const Domain& dom, int r0, int n) {
    const AdjointPlan& plan = *c.plan;
    const int L = lanes(c);
    for (int i = 0; i < n; ++i) {
      const size_t v = idx(dom.value_base) + static_cast<size_t>(r0 + i);
      double* dst = c.vbar + v * static_cast<size_t>(L);
      for (int l = 0; l < L; ++l) dst[l] = 0.0;
      for (std::int32_t e = plan.output_offsets[v]; e < plan.output_offsets[v + 1]; ++e) {
        const double* src = c.out_bar + idx(plan.output_readers[idx(e)]) * static_cast<size_t>(c.B) + static_cast<size_t>(c.b0);
        for (int l = 0; l < L; ++l) dst[l] += src[l];
      }
      for (std::int32_t e = plan.gather_offsets[v]; e < plan.gather_offsets[v + 1]; ++e) {
        const double* src = c.gbar + idx(plan.gather_readers[idx(e)]) * static_cast<size_t>(L);
        for (int l = 0; l < L; ++l) dst[l] += src[l];
      }
      for (std::int32_t e = plan.sum_offsets[v]; e < plan.sum_offsets[v + 1]; ++e) {
        const double* src = c.segbar + idx(plan.sum_readers[idx(e)]) * static_cast<size_t>(L);
        for (int l = 0; l < L; ++l) dst[l] += src[l];
      }
      for (std::int32_t e = plan.affine_offsets[v]; e < plan.affine_offsets[v + 1]; ++e) {
        const double coef = plan.affine_coefs[idx(e)];
        const double* src = c.segbar + idx(plan.affine_readers[idx(e)]) * static_cast<size_t>(L);
        for (int l = 0; l < L; ++l) dst[l] += coef * src[l];
      }
    }
  }

  // The reverse pass of the chunk: domains backwards; per tile pull v̄, recompute the
  // intermediate steps, zero the tile's edge slots and step adjoints, run the rules last to first.
  static void reverse(const Ctx& c) {
    const Program& p = *c.p;
    const AdjointPlan& plan = *c.plan;
    const int L = lanes(c);
    for (size_t k = 0; k < p.inputs.size(); ++k) {
      double* dst = c.state_bar + k * static_cast<size_t>(c.B) + static_cast<size_t>(c.b0);
      for (int l = 0; l < L; ++l) dst[l] = 0.0;
    }
    for (size_t d = p.domains.size(); d-- > 0;) {
      const Domain& dom = p.domains[d];
      const Group& g = p.groups[d];
      const AdjointPlan::DomainPlan& dp = plan.domains[d];
      const int last = static_cast<int>(g.steps.size()) - 1;
      const double* dom_values = c.values + idx(dom.value_base) * static_cast<size_t>(L);
      // The reverse scan (D41): a scan's rows one at a time from the last to the first, so
      // that row r's pull sees the edge slot (carry, r + 1) its successor's reverse has just
      // written — the carried adjoint. Rows of other domains are independent and go in tiles.
      // Mutant adjoint.scan_forward_order: the scan's rows are reversed in row order, so the
      // carried adjoint arrives after it was pulled.
      const int n_tiles = dp.is_scan ? dom.rows : (dom.rows + c.tile - 1) / c.tile;
      for (int ti = 0; ti < n_tiles; ++ti) {
        const int r0 = dp.is_scan ? ((mutation::compiled_in && c.scan_forward_order) ? ti : dom.rows - 1 - ti) : ti * c.tile;
        const int n = dp.is_scan ? 1 : std::min(c.tile, dom.rows - r0);
        const size_t N = static_cast<size_t>(n) * static_cast<size_t>(L);
        pull(c, dom, r0, n);
        if (dp.is_const) continue;
        // Recompute steps 0 .. last-1 (their operands stay loaded); load the last step's operands.
        for (int k = 0; k < last; ++k) {
          forward_step(c, static_cast<int>(d), g.steps[idx(k)], k, r0, n, c.fs + static_cast<size_t>(k) * c.tile_elems);
        }
        {
          const Step& s = g.steps[idx(last)];
          if (op_arity(s.op) > 0) {  // the rules of Sum / Affine / Const / Input read no operand vector
            operand(c, s.a, last, 0, r0, n, true);
            operand(c, s.b, last, 1, r0, n, true);
            operand(c, s.c, last, 2, r0, n, true);
          }
        }
        for (int k = 0; k < last; ++k) std::memset(c.sbar + static_cast<size_t>(k) * c.tile_elems, 0, N * sizeof(double));
        for (std::int32_t gi : dp.gathers) {
          std::memset(c.gbar + (idx(plan.gather_slot_base[idx(gi)]) + static_cast<size_t>(r0)) * static_cast<size_t>(L), 0,
                      N * sizeof(double));
        }
        for (std::int32_t si : dp.segments) {
          std::memset(c.segbar + (idx(plan.segment_slot_base[idx(si)]) + static_cast<size_t>(r0)) * static_cast<size_t>(L), 0,
                      N * sizeof(double));
        }
        for (int k = last; k >= 0; --k) {
          const double* yb = k == last ? c.vbar + (idx(dom.value_base) + static_cast<size_t>(r0)) * static_cast<size_t>(L)
                                       : c.sbar + static_cast<size_t>(k) * c.tile_elems;
          const double* y = k == last ? dom_values + static_cast<size_t>(r0) * static_cast<size_t>(L)
                                      : c.fs + static_cast<size_t>(k) * c.tile_elems;
          reverse_step(c, static_cast<int>(d), g.steps[idx(k)], k, r0, n, yb, y);
        }
      }
    }
  }

  static void run_chunk(const Ctx& c) {
    forward(c);
    reverse(c);
  }
};

}  // namespace

struct Adjoint::Impl {
  const Program* p = nullptr;
  Options opt;
  AdjointPlan plan;
  int Lt = 0;              // lanes the buffers hold: min(lane_tile, max_batch)
  size_t tile_elems = 0;   // tile · Lt
  std::vector<double> values, vbar, gbar, segbar, fs, sbar, ops;
  // M4/C1: per domain, the catalogued kernel and its binding, or {nullptr, {}} — see forward()
  // above. Populated once here, in domain order, when opt.use_catalogue is true.
  std::vector<catalogue::Kernel> cat_kernels;
  std::vector<catalogue::DomainBinding> cat_bindings;
  std::size_t cat_candidates = 0, cat_groups = 0, cat_candidate_rows = 0, cat_rows = 0;
#ifdef EPYKOS_EXEC_PROFILE
  double cat_ns = 0.0, total_ns = 0.0;
#endif
};

Adjoint::Adjoint(const Program& program, Options options) : impl_(std::make_unique<Impl>()) {
  Impl& im = *impl_;
  if (options.tile < 1) throw std::invalid_argument("adjoint: tile must be >= 1");
  if (options.max_batch < 1) throw std::invalid_argument("adjoint: max_batch must be >= 1");
  if (options.lane_tile < 1) throw std::invalid_argument("adjoint: lane_tile must be >= 1");
  im.p = &program;
  im.opt = options;
  im.plan = build_plan(program);
  im.Lt = std::min(options.lane_tile, options.max_batch);
  im.tile_elems = static_cast<size_t>(options.tile) * static_cast<size_t>(im.Lt);
  const size_t Lt = static_cast<size_t>(im.Lt);
  im.values.assign(im.plan.num_values * Lt, 0.0);
  im.vbar.assign(im.plan.num_values * Lt, 0.0);
  im.gbar.assign(static_cast<size_t>(im.plan.n_gather_slots) * Lt, 0.0);
  im.segbar.assign(static_cast<size_t>(im.plan.n_segment_slots) * Lt, 0.0);
  const size_t steps = static_cast<size_t>(std::max(im.plan.max_steps, 1));
  im.fs.assign(steps * im.tile_elems, 0.0);
  im.sbar.assign(steps * im.tile_elems, 0.0);
  im.ops.assign(3 * steps * im.tile_elems, 0.0);

  im.cat_kernels.assign(program.domains.size(), nullptr);
  im.cat_bindings.resize(program.domains.size());
  if (options.use_catalogue) {
    for (std::size_t d = 0; d < program.domains.size(); ++d) {
      const ir::domain_id did = static_cast<ir::domain_id>(d);
      if (!catalogue::is_cataloguable(program, did)) continue;
      ++im.cat_candidates;
      im.cat_candidate_rows += static_cast<std::size_t>(program.domains[d].rows);
      const catalogue::Signature sig = catalogue::signature_of(program, did);
      if (const catalogue::Kernel k = catalogue::lookup(sig)) {
        im.cat_kernels[d] = k;
        im.cat_bindings[d] = catalogue::bind_domain(program, did);
        ++im.cat_groups;
        im.cat_rows += static_cast<std::size_t>(program.domains[d].rows);
      }
    }
  }
}

Adjoint::~Adjoint() = default;

void Adjoint::run(const double* state, int B, const double* out_bar, double* out, double* state_bar) const {
  const Impl& im = *impl_;
  if (B < 1 || B > im.opt.max_batch) {
    throw std::invalid_argument("adjoint: B must be in [1, max_batch] (" + std::to_string(B) + " vs " +
                                std::to_string(im.opt.max_batch) + ")");
  }
  // The buffers are the object's own scratch (run() is const in the interface sense only); the
  // Impl behind the unique_ptr is never a const object.
  Impl& m = *impl_;
  Ctx c;
  c.p = im.p;
  c.plan = &im.plan;
  c.values = m.values.data();
  c.vbar = m.vbar.data();
  c.gbar = m.gbar.data();
  c.segbar = m.segbar.data();
  c.fs = m.fs.data();
  c.sbar = m.sbar.data();
  c.ops = m.ops.data();
  c.tile_elems = im.tile_elems;
  c.tile = im.opt.tile;
  c.state = state;
  c.out_bar = out_bar;
  c.out = out;
  c.state_bar = state_bar;
  c.B = B;
  c.select_wrong_arm = mutant("adjoint.select_wrong_arm");
  c.recip_rule_sign = mutant("adjoint.recip_rule_sign");
  c.scan_forward_order = mutant("adjoint.scan_forward_order");
  c.cat_kernels = m.cat_kernels.empty() ? nullptr : m.cat_kernels.data();
  c.cat_bindings = m.cat_bindings.empty() ? nullptr : m.cat_bindings.data();
#ifdef EPYKOS_EXEC_PROFILE
  c.cat_ns = &m.cat_ns;
  c.total_ns = &m.total_ns;
#endif
  for (int b0 = 0; b0 < B; b0 += im.Lt) {
    c.b0 = b0;
    c.L = std::min(im.Lt, B - b0);
    switch (c.L) {
      case 1: Lanes<1>::run_chunk(c); break;
      case 4: Lanes<4>::run_chunk(c); break;
      case 8: Lanes<8>::run_chunk(c); break;
      case 16: Lanes<16>::run_chunk(c); break;
      case 32: Lanes<32>::run_chunk(c); break;
      case 64: Lanes<64>::run_chunk(c); break;
      default: Lanes<0>::run_chunk(c); break;
    }
  }
}

std::string Adjoint::describe() const {
  const Impl& im = *impl_;
  std::ostringstream os;
  os << "adjoint: tile " << im.opt.tile << " rows, lane_tile " << im.opt.lane_tile << " (buffers hold " << im.Lt
     << " lanes), max_batch " << im.opt.max_batch << "; value buffers " << value_bytes() << " bytes, edge buffers "
     << edge_bytes() << " bytes, scratch " << scratch_bytes() << " bytes, tables " << table_bytes() << " bytes\n";
  os << adjoint::describe(im.plan, *im.p);
  return os.str();
}

const AdjointPlan& Adjoint::plan() const noexcept { return impl_->plan; }
const Options& Adjoint::options() const noexcept { return impl_->opt; }
const Program& Adjoint::program() const noexcept { return *impl_->p; }
int Adjoint::n_inputs() const noexcept { return static_cast<int>(impl_->p->inputs.size()); }
int Adjoint::n_outputs() const noexcept { return static_cast<int>(impl_->p->outputs.size()); }
int Adjoint::max_batch() const noexcept { return impl_->opt.max_batch; }
std::size_t Adjoint::num_values() const noexcept { return impl_->plan.num_values; }
std::size_t Adjoint::value_bytes() const noexcept { return (impl_->values.size() + impl_->vbar.size()) * sizeof(double); }
std::size_t Adjoint::edge_bytes() const noexcept { return (impl_->gbar.size() + impl_->segbar.size()) * sizeof(double); }
std::size_t Adjoint::scratch_bytes() const noexcept {
  return (impl_->fs.size() + impl_->sbar.size() + impl_->ops.size()) * sizeof(double);
}
std::size_t Adjoint::table_bytes() const noexcept { return impl_->plan.table_bytes(); }

catalogue::Coverage Adjoint::coverage() const noexcept {
  const Impl& im = *impl_;
  catalogue::Coverage c;
  c.groups_total = im.cat_candidates;
  c.groups_catalogued = im.cat_groups;
  c.rows_total = im.cat_candidate_rows;
  c.rows_catalogued = im.cat_rows;
#ifdef EPYKOS_EXEC_PROFILE
  if (im.total_ns > 0.0) c.time_fraction = im.cat_ns / im.total_ns;
#endif
  return c;
}

}  // namespace epykos::adjoint
