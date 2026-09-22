// The hand-fused M1 reference kernel (M1/P5). See the header for the design.
//
// Contraction is off in this TU whatever the preset, so the reference-arithmetic mode is bitwise
// the contraction-free oracle and the fused mode's only fused operations are its explicit
// std::fma calls (GCC in ISO mode never contracts unless asked; the pragma covers clang).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include "epykos/hand/m1_hand_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "epykos/hand/exp_poly.hpp"
#include "epykos/maths/m1/curve.hpp"

namespace epykos::hand {

namespace {

// Lane tile of the batched coupon pass: 8 doubles = two AVX2 vectors, four NEON vectors. The leg
// accumulators of a tile live in registers across the rows of a swap.
constexpr int kLaneTile = 8;

std::size_t idx(int i) { return static_cast<std::size_t>(i); }

int round_up(int n, int m) { return ((n + m - 1) / m) * m; }

}  // namespace

int M1HandKernel::lane_tile() const noexcept { return kLaneTile; }

M1HandKernel::M1HandKernel(const m1::Book& book, M1HandOptions options) : opt_(options) {
  if (opt_.arith == HandArith::reference) {
    opt_.shared_reciprocal = false;
    opt_.exp = HandExp::std_exp;
  }
  if (opt_.max_batch < 1) throw std::invalid_argument("M1HandKernel: max_batch must be >= 1");
  n_swaps_ = book.n_swaps;
  n_knots_ = m1::n_knots;

  // ---- unique discount times: every coupon end, plus every non-realised float coupon start.
  std::vector<double> times;
  times.reserve(idx(book.n_rows) * 2);
  for (int r = 0; r < book.n_rows; ++r) {
    times.push_back(book.row_t_end[idx(r)]);
    if (book.row_leg[idx(r)] == m1::float_leg && !book.row_is_realised_first[idx(r)]) {
      times.push_back(book.row_t_start[idx(r)]);
    }
  }
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());
  time_ = times;
  n_times_ = static_cast<int>(time_.size());
  auto time_index = [&](double t) {
    const auto it = std::lower_bound(time_.begin(), time_.end(), t);
    if (it == time_.end() || *it != t) throw std::logic_error("M1HandKernel: time not in table");
    return static_cast<int>(it - time_.begin());
  };

  // ---- W: the weights of zero_rate() in curve.hpp, computed with the same expressions so that
  // w0·z[k0] + w1·z[k1] is the oracle's (1 − w)·z[k] + w·z[k+1] bitwise, and z[k] exactly at the
  // knots and beyond the ends (1.0·z + 0.0·z).
  knot0_.resize(idx(n_times_));
  knot1_.resize(idx(n_times_));
  w0_.resize(idx(n_times_));
  w1_.resize(idx(n_times_));
  const double* kt = book.knot_t.data();
  const int n = n_knots_;
  for (int u = 0; u < n_times_; ++u) {
    const double t = time_[idx(u)];
    int k0, k1;
    double w0, w1;
    if (t <= kt[0]) {
      k0 = k1 = 0;
      w0 = 1.0;
      w1 = 0.0;
    } else if (t >= kt[n - 1]) {
      k0 = k1 = n - 1;
      w0 = 1.0;
      w1 = 0.0;
    } else {
      int k = 0;
      while (t >= kt[k + 1]) ++k;
      if (t == kt[k]) {
        k0 = k1 = k;
        w0 = 1.0;
        w1 = 0.0;
      } else {
        const double w = (t - kt[k]) / (kt[k + 1] - kt[k]);
        k0 = k;
        k1 = k + 1;
        w0 = 1.0 - w;
        w1 = w;
      }
    }
    knot0_[idx(u)] = k0;
    knot1_[idx(u)] = k1;
    w0_[idx(u)] = w0;
    w1_[idx(u)] = w1;
  }

  // ---- coupon rows, bucketed by kind, in swap order and period order within a leg.
  plain_begin_.assign(idx(n_swaps_ + 1), 0);
  fixed_end_.assign(idx(n_swaps_), 0);
  float_begin_.assign(idx(n_swaps_ + 1), 0);
  side_.resize(idx(n_swaps_));
  for (int i = 0; i < n_swaps_; ++i) {
    const int T = book.tenor[idx(i)];
    const int seasoned = book.seasoned[idx(i)] ? 1 : 0;
    plain_begin_[idx(i + 1)] = plain_begin_[idx(i)] + T + seasoned;
    fixed_end_[idx(i)] = plain_begin_[idx(i)] + T;
    float_begin_[idx(i + 1)] = float_begin_[idx(i)] + T - seasoned;
    side_[idx(i)] = static_cast<double>(book.side[idx(i)]);
  }
  n_plain_ = plain_begin_[idx(n_swaps_)];
  n_float_ = float_begin_[idx(n_swaps_)];
  plain_time_.resize(idx(n_plain_));
  plain_coef_.resize(idx(n_plain_));
  float_s_.resize(idx(n_float_));
  float_e_.resize(idx(n_float_));
  float_coef_.resize(idx(n_float_));
  float_tau_.resize(idx(n_float_));

  const bool ref = opt_.arith == HandArith::reference;
  for (int i = 0; i < n_swaps_; ++i) {
    const int T = book.tenor[idx(i)];
    const double N = book.notional[idx(i)];
    const double K = book.fixed_rate[idx(i)];
    const double R = book.realised_rate[idx(i)];
    int p = plain_begin_[idx(i)];
    // Fixed leg: coef = N·τ·K in the oracle's order ((N·τ)·K).
    const int f0 = m1::fixed_row_begin(book, i);
    for (int j = 0; j < T; ++j, ++p) {
      const std::size_t r = idx(f0 + j);
      plain_time_[idx(p)] = time_index(book.row_t_end[r]);
      plain_coef_[idx(p)] = N * book.row_tau[r] * K;
    }
    // Float leg: the realised first coupon is a plain row with coef = (N·τ)·R; the others carry a
    // forward.
    const int g0 = m1::float_row_begin(book, i);
    int q = float_begin_[idx(i)];
    for (int j = 0; j < T; ++j) {
      const std::size_t r = idx(g0 + j);
      if (book.row_is_realised_first[r]) {
        if (j != 0) throw std::logic_error("M1HandKernel: realised fixing off the first row");
        plain_time_[idx(p)] = time_index(book.row_t_end[r]);
        plain_coef_[idx(p)] = N * book.row_tau[r] * R;
        ++p;
        continue;
      }
      float_s_[idx(q)] = time_index(book.row_t_start[r]);
      float_e_[idx(q)] = time_index(book.row_t_end[r]);
      float_coef_[idx(q)] = N * book.row_tau[r];
      float_tau_[idx(q)] = ref ? book.row_tau[r] : 1.0 / book.row_tau[r];
      ++q;
    }
    if (p != plain_begin_[idx(i + 1)] || q != float_begin_[idx(i + 1)]) {
      throw std::logic_error("M1HandKernel: row count mismatch");
    }
  }

  // ---- scratch, sized once.
  max_stride_ = round_up(opt_.max_batch, kLaneTile);
  zp_.assign(idx(n_knots_) * idx(max_stride_), 0.0);
  df_.assign(idx(n_times_) * idx(max_stride_), 0.0);
  rcp_.assign(opt_.shared_reciprocal ? idx(n_times_) * idx(max_stride_) : 0, 0.0);
  acc_.assign(idx(max_stride_), 0.0);
}

// DF (and 1/DF) at every unique time for `stride` lanes of zp_, batch innermost.
void M1HandKernel::curve_pass(int stride) const {
  const double* zp = zp_.data();
  double* df = df_.data();
  const int nu = n_times_;
  const std::size_t st = idx(stride);
  if (opt_.exp == HandExp::poly) {
    for (int u = 0; u < nu; ++u) {
      const double* z0 = zp + idx(knot0_[idx(u)]) * st;
      const double* z1 = zp + idx(knot1_[idx(u)]) * st;
      const double a = w0_[idx(u)], b = w1_[idx(u)], t = time_[idx(u)];
      double* d = df + idx(u) * st;
      for (int l = 0; l < stride; ++l) {
        const double zt = a * z0[l] + b * z1[l];
        d[l] = exp_poly((-zt) * t);
      }
    }
  } else {
    for (int u = 0; u < nu; ++u) {
      const double* z0 = zp + idx(knot0_[idx(u)]) * st;
      const double* z1 = zp + idx(knot1_[idx(u)]) * st;
      const double a = w0_[idx(u)], b = w1_[idx(u)], t = time_[idx(u)];
      double* d = df + idx(u) * st;
      for (int l = 0; l < stride; ++l) {
        const double zt = a * z0[l] + b * z1[l];
        d[l] = std::exp((-zt) * t);
      }
    }
  }
  if (opt_.shared_reciprocal) {
    double* rcp = rcp_.data();
    const int total = nu * stride;
    for (int l = 0; l < total; ++l) rcp[l] = 1.0 / df[l];
  }
}

// The fused coupon → leg → swap pass over lane tiles of L. Shared: DF(s)·(1/DF(e)) with the
// precomputed reciprocal; Ref: the oracle's operation order (÷, no fma).
template <int L, bool Shared, bool Ref>
void M1HandKernel::coupon_pass(int stride, int B, double* swap_pv) const {
  const double* df = df_.data();
  const double* rcp = rcp_.data();
  const int* plain_begin = plain_begin_.data();
  const int* fixed_end = fixed_end_.data();
  const int* plain_time = plain_time_.data();
  const double* plain_coef = plain_coef_.data();
  const int* float_begin = float_begin_.data();
  const int* float_s = float_s_.data();
  const int* float_e = float_e_.data();
  const double* float_coef = float_coef_.data();
  const double* float_tau = float_tau_.data();
  const double* side = side_.data();
  const std::size_t st = idx(stride);
  const int n_tiles = stride / L;

  for (int i = 0; i < n_swaps_; ++i) {
    const int pb = plain_begin[i], fe = fixed_end[i], pe = plain_begin[i + 1];
    const int qb = float_begin[i], qe = float_begin[i + 1];
    const double s = side[i];
    for (int tile = 0; tile < n_tiles; ++tile) {
      const int lane0 = tile * L;
      double fixed[L], flt[L];
      for (int l = 0; l < L; ++l) {
        fixed[l] = 0.0;
        flt[l] = 0.0;
      }
      // Fixed leg: pv_j = coef_j·DF(e_j).
      for (int p = pb; p < fe; ++p) {
        const double* de = df + idx(plain_time[p]) * st + lane0;
        const double c = plain_coef[p];
        for (int l = 0; l < L; ++l) fixed[l] = Ref ? fixed[l] + c * de[l] : std::fma(c, de[l], fixed[l]);
      }
      // Realised first float coupon (0 or 1 row): pv_1 = coef·DF(e_1).
      for (int p = fe; p < pe; ++p) {
        const double* de = df + idx(plain_time[p]) * st + lane0;
        const double c = plain_coef[p];
        for (int l = 0; l < L; ++l) flt[l] = Ref ? flt[l] + c * de[l] : std::fma(c, de[l], flt[l]);
      }
      // Float coupons: fwd = (DF(s)/DF(e) − 1)/τ, pv = (N·τ·fwd)·DF(e).
      for (int q = qb; q < qe; ++q) {
        const double* ds = df + idx(float_s[q]) * st + lane0;
        const double* de = df + idx(float_e[q]) * st + lane0;
        const double c = float_coef[q];
        const double tf = float_tau[q];  // τ (Ref) or 1/τ
        if constexpr (Ref) {
          for (int l = 0; l < L; ++l) {
            const double fwd = (ds[l] / de[l] - 1.0) / tf;
            flt[l] = flt[l] + (c * fwd) * de[l];
          }
        } else if constexpr (Shared) {
          const double* re = rcp + idx(float_e[q]) * st + lane0;
          for (int l = 0; l < L; ++l) {
            const double x = std::fma(ds[l], re[l], -1.0);
            flt[l] = std::fma(c * (x * tf), de[l], flt[l]);
          }
        } else {
          for (int l = 0; l < L; ++l) {
            const double x = ds[l] / de[l] - 1.0;
            flt[l] = std::fma(c * (x * tf), de[l], flt[l]);
          }
        }
      }
      // Swap pv = side·(fixed − float) for the lanes of this tile that exist.
      const int w = std::min(L, B - lane0);
      double* out = swap_pv + idx(i) * idx(B) + lane0;
      for (int l = 0; l < w; ++l) out[l] = s * (fixed[l] - flt[l]);
    }
  }
}

template <int L>
void M1HandKernel::eval_impl(const double* z, int B, double* swap_pv, double* book_pv) const {
  const int stride = round_up(B, L);
  // State in, batch innermost, padded lanes repeating the last state so exp sees finite input.
  double* zp = zp_.data();
  for (int k = 0; k < n_knots_; ++k) {
    const double* zk = z + idx(k) * idx(B);
    double* out = zp + idx(k) * idx(stride);
    for (int l = 0; l < stride; ++l) out[l] = zk[std::min(l, B - 1)];
  }
  curve_pass(stride);
  if (opt_.arith == HandArith::reference) {
    coupon_pass<L, false, true>(stride, B, swap_pv);
  } else if (opt_.shared_reciprocal) {
    coupon_pass<L, true, false>(stride, B, swap_pv);
  } else {
    coupon_pass<L, false, false>(stride, B, swap_pv);
  }
  // Book pv: left fold over swaps, per lane.
  double* acc = acc_.data();
  for (int b = 0; b < B; ++b) acc[b] = swap_pv[b];
  for (int i = 1; i < n_swaps_; ++i) {
    const double* pv = swap_pv + idx(i) * idx(B);
    for (int b = 0; b < B; ++b) acc[b] = acc[b] + pv[b];
  }
  for (int b = 0; b < B; ++b) book_pv[b] = acc[b];
}

void M1HandKernel::eval(const double* z, double* swap_pv, double* book_pv) const {
  eval_impl<1>(z, 1, swap_pv, book_pv);
}

void M1HandKernel::eval_batch(const double* z, int B, double* swap_pv, double* book_pv) const {
  if (B < 1 || B > opt_.max_batch) throw std::invalid_argument("M1HandKernel::eval_batch: B out of range");
  eval_impl<kLaneTile>(z, B, swap_pv, book_pv);
}

}  // namespace epykos::hand
