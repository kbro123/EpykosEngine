// The hand-fused M1 reference kernel (M1/P5). See the header for the design.
//
// An E0 TU (src/**/*_e0.cpp, root CMakeLists.txt): compiled with -ffp-contract=off in every
// preset on every compiler, so the reference-arithmetic mode is bitwise the contraction-free
// oracle and the fused mode's only fused operations are its explicit std::fma calls (D13, D25).
#ifndef EPYKOS_FP_CONTRACT_OFF
#error "m1_hand_kernel_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in CMakeLists.txt)"
#endif


#include "epykos/hand/m1_hand_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "epykos/hand/exp_poly.hpp"
#include "epykos/maths/m1/curve.hpp"

namespace epykos::hand {

namespace {

// Lane tile of the batched coupon pass: 32 doubles = eight AVX2 vectors (sixteen NEON) per leg
// accumulator, held in registers across the rows of a swap (measured against 8 and 16).
constexpr int kLaneTile = 32;

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
  n_times_ = static_cast<int>(times.size());
  // Numbering: first use along the processing order (assigned below, after the order is known).
  std::vector<int> number_of_sorted(idx(n_times_), -1);
  time_.clear();
  time_.reserve(idx(n_times_));
  auto time_index = [&](double t) {
    const auto it = std::lower_bound(times.begin(), times.end(), t);
    if (it == times.end() || *it != t) throw std::logic_error("M1HandKernel: time not in table");
    int& num = number_of_sorted[idx(static_cast<int>(it - times.begin()))];
    if (num < 0) {
      num = static_cast<int>(time_.size());
      time_.push_back(t);
    }
    return num;
  };

  // ---- processing order: swaps grouped by (tenor, seasoned); stable within a group.
  swap_of_.resize(idx(n_swaps_));
  for (int i = 0; i < n_swaps_; ++i) swap_of_[idx(i)] = i;
  std::stable_sort(swap_of_.begin(), swap_of_.end(), [&](int a, int b) {
    const int ta = book.tenor[idx(a)], tb = book.tenor[idx(b)];
    if (ta != tb) return ta > tb;  // longest first
    return book.seasoned[idx(a)] < book.seasoned[idx(b)];
  });
  group_begin_.clear();
  group_tenor_.clear();
  group_seasoned_.clear();
  for (int pos = 0; pos < n_swaps_; ++pos) {
    const int i = swap_of_[idx(pos)];
    const int T = book.tenor[idx(i)];
    const int sea = book.seasoned[idx(i)] ? 1 : 0;
    if (pos == 0 || T != group_tenor_.back() || sea != group_seasoned_.back()) {
      group_begin_.push_back(pos);
      group_tenor_.push_back(T);
      group_seasoned_.push_back(sea);
    }
  }
  group_begin_.push_back(n_swaps_);
  n_groups_ = static_cast<int>(group_tenor_.size());

  // ---- coupon rows, bucketed by kind, in processing order and period order within a leg.
  plain_begin_.assign(idx(n_swaps_ + 1), 0);
  float_begin_.assign(idx(n_swaps_ + 1), 0);
  side_.resize(idx(n_swaps_));
  for (int pos = 0; pos < n_swaps_; ++pos) {
    const int i = swap_of_[idx(pos)];
    const int T = book.tenor[idx(i)];
    const int seasoned = book.seasoned[idx(i)] ? 1 : 0;
    plain_begin_[idx(pos + 1)] = plain_begin_[idx(pos)] + T + seasoned;
    float_begin_[idx(pos + 1)] = float_begin_[idx(pos)] + T - seasoned;
    side_[idx(pos)] = static_cast<double>(book.side[idx(i)]);
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
  for (int pos = 0; pos < n_swaps_; ++pos) {
    const int i = swap_of_[idx(pos)];
    const int T = book.tenor[idx(i)];
    const double N = book.notional[idx(i)];
    const double K = book.fixed_rate[idx(i)];
    const double R = book.realised_rate[idx(i)];
    int p = plain_begin_[idx(pos)];
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
    int q = float_begin_[idx(pos)];
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
    if (p != plain_begin_[idx(pos + 1)] || q != float_begin_[idx(pos + 1)]) {
      throw std::logic_error("M1HandKernel: row count mismatch");
    }
  }

  if (static_cast<int>(time_.size()) != n_times_) throw std::logic_error("M1HandKernel: unused time");

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

  // ---- premultiplied row offsets for the batch path (stride max_stride_).
  max_stride_ = round_up(opt_.max_batch, kLaneTile);
  plain_off_.resize(idx(n_plain_));
  for (int p = 0; p < n_plain_; ++p) plain_off_[idx(p)] = plain_time_[idx(p)] * max_stride_;
  float_s_off_.resize(idx(n_float_));
  float_e_off_.resize(idx(n_float_));
  for (int q = 0; q < n_float_; ++q) {
    float_s_off_[idx(q)] = float_s_[idx(q)] * max_stride_;
    float_e_off_[idx(q)] = float_e_[idx(q)] * max_stride_;
  }

  // ---- scratch, sized once.
  zp_.assign(idx(n_knots_) * idx(max_stride_), 0.0);
  df_.assign(idx(n_times_) * idx(max_stride_), 0.0);
  rcp_.assign(opt_.shared_reciprocal ? idx(n_times_) * idx(max_stride_) : 0, 0.0);
  max_group_ = 0;
  for (int g = 0; g < n_groups_; ++g) max_group_ = std::max(max_group_, group_begin_[idx(g + 1)] - group_begin_[idx(g)]);
  leg_.assign(idx(max_group_) * idx(kLaneTile), 0.0);
  acc_.assign(idx(max_stride_), 0.0);
}

// DF (and 1/DF) at every unique time for the lanes of zp_, batch innermost. The exp runs over the
// flat [times × stride] array so it vectorises across times at B = 1 and across lanes in a batch
// alike; per element the operations are identical either way.
template <int L>
void M1HandKernel::curve_pass() const {
  const double* zp = zp_.data();
  double* df = df_.data();
  const int nu = n_times_;
  const int stride = L == 1 ? 1 : max_stride_;
  const std::size_t st = idx(stride);
  // 1. x = (−z(t))·t through W.
  for (int u = 0; u < nu; ++u) {
    const double* z0 = zp + idx(knot0_[idx(u)]) * st;
    const double* z1 = zp + idx(knot1_[idx(u)]) * st;
    const double a = w0_[idx(u)], b = w1_[idx(u)], t = time_[idx(u)];
    double* x = df + idx(u) * st;
    for (int l = 0; l < stride; ++l) {
      const double zt = a * z0[l] + b * z1[l];
      x[l] = (-zt) * t;
    }
  }
  // 2. DF = exp(x) in place.
  const int total = nu * stride;
  if (opt_.exp == HandExp::poly) {
    exp_poly_array(df, df, total);
  } else {
    for (int i = 0; i < total; ++i) df[i] = std::exp(df[i]);
  }
  // 3. 1/DF once per (time, lane).
  if (opt_.shared_reciprocal) {
    double* rcp = rcp_.data();
    for (int i = 0; i < total; ++i) rcp[i] = 1.0 / df[i];
  }
}

// The fused coupon → leg → swap pass. Per (tenor, seasoned) group and lane tile of L: first the
// fixed legs of the group's swaps (T plain rows each) into a small scratch, then the float legs
// (the realised row if the group is seasoned, then the forward rows) in registers, and
// side·(fixed − float) scattered to each swap's slot. Two loops rather than one so that each has
// only L/4 vector accumulators live (the float rows also need temporaries) and the compiler keeps
// them in registers. Every fold is left to right in period order. Shared: DF(s)·(1/DF(e)) with the
// precomputed reciprocal; Ref: the oracle's operation order (÷, no fma).
template <int L, bool Shared, bool Ref>
void M1HandKernel::coupon_pass(int B, double* swap_pv) const {
  const double* df = df_.data();
  const double* rcp = rcp_.data();
  const int* plain_begin = plain_begin_.data();
  const int* plain_time = L == 1 ? plain_time_.data() : plain_off_.data();
  const double* plain_coef = plain_coef_.data();
  const int* float_begin = float_begin_.data();
  const int* float_s = L == 1 ? float_s_.data() : float_s_off_.data();
  const int* float_e = L == 1 ? float_e_.data() : float_e_off_.data();
  const double* float_coef = float_coef_.data();
  const double* float_tau = float_tau_.data();
  const double* side = side_.data();
  const int* swap_of = swap_of_.data();
  double* leg = leg_.data();
  const int stride = L == 1 ? 1 : max_stride_;
  const int n_tiles = stride / L;

  for (int g = 0; g < n_groups_; ++g) {
    const int pos0 = group_begin_[idx(g)], pos1 = group_begin_[idx(g + 1)];
    const int T = group_tenor_[idx(g)];
    const int seasoned = group_seasoned_[idx(g)];
    const int n_float = T - seasoned;
    for (int tile = 0; tile < n_tiles; ++tile) {
      const int lane0 = tile * L;
      const int w = std::min(L, B - lane0);
      // Fixed legs: pv_j = coef_j·DF(e_j), into leg[(pos − pos0)·L + l].
      for (int pos = pos0; pos < pos1; ++pos) {
        const int pb = plain_begin[pos];
        double fixed[L];
        for (int l = 0; l < L; ++l) fixed[l] = 0.0;
        for (int j = 0; j < T; ++j) {
          const int p = pb + j;
          const double* de = df + idx(plain_time[p]) + lane0;
          const double c = plain_coef[p];
          for (int l = 0; l < L; ++l) fixed[l] = Ref ? fixed[l] + c * de[l] : std::fma(c, de[l], fixed[l]);
        }
        double* out = leg + idx(pos - pos0) * L;
        for (int l = 0; l < L; ++l) out[l] = fixed[l];
      }
      // Float legs, then the swap pv.
      for (int pos = pos0; pos < pos1; ++pos) {
        const int pb = plain_begin[pos];
        const int qb = float_begin[pos];
        double flt[L];
        for (int l = 0; l < L; ++l) flt[l] = 0.0;
        // Realised first float coupon of a seasoned group: pv_1 = coef·DF(e_1).
        if (seasoned) {
          const int p = pb + T;
          const double* de = df + idx(plain_time[p]) + lane0;
          const double c = plain_coef[p];
          for (int l = 0; l < L; ++l) flt[l] = Ref ? flt[l] + c * de[l] : std::fma(c, de[l], flt[l]);
        }
        // Float coupons: fwd = (DF(s)/DF(e) − 1)/τ, pv = (N·τ·fwd)·DF(e).
        for (int j = 0; j < n_float; ++j) {
          const int q = qb + j;
          const double* ds = df + idx(float_s[q]) + lane0;
          const double* de = df + idx(float_e[q]) + lane0;
          const double c = float_coef[q];
          const double tf = float_tau[q];  // τ (Ref) or 1/τ
          if constexpr (Ref) {
            for (int l = 0; l < L; ++l) {
              const double fwd = (ds[l] / de[l] - 1.0) / tf;
              flt[l] = flt[l] + (c * fwd) * de[l];
            }
          } else if constexpr (Shared) {
            const double* re = rcp + idx(float_e[q]) + lane0;
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
        // Swap pv = side·(fixed − float), scattered to the swap's slot, lanes that exist only.
        const double s = side[pos];
        const double* fixed = leg + idx(pos - pos0) * L;
        double* out = swap_pv + idx(swap_of[pos]) * idx(B) + lane0;
        for (int l = 0; l < w; ++l) out[l] = s * (fixed[l] - flt[l]);
      }
    }
  }
}

template <int L>
void M1HandKernel::eval_impl(const double* z, int B, double* swap_pv, double* book_pv) const {
  const int stride = L == 1 ? 1 : max_stride_;
  // State in, batch innermost, padded lanes repeating the last state so exp sees finite input.
  double* zp = zp_.data();
  for (int k = 0; k < n_knots_; ++k) {
    const double* zk = z + idx(k) * idx(B);
    double* out = zp + idx(k) * idx(stride);
    for (int l = 0; l < stride; ++l) out[l] = zk[std::min(l, B - 1)];
  }
  curve_pass<L>();
  if (opt_.arith == HandArith::reference) {
    coupon_pass<L, false, true>(B, swap_pv);
  } else if (opt_.shared_reciprocal) {
    coupon_pass<L, true, false>(B, swap_pv);
  } else {
    coupon_pass<L, false, false>(B, swap_pv);
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
