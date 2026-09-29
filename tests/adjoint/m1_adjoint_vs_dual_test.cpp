// M2/Q3b gate (docs/WORKLOADS.md §M2 "Adjoint: ... vs forward mode (1e-12)"; RESUME.md §3 M2 Q3
// "vs Dual 1e-12"): the mechanical adjoint against forward mode on the M1 book.
//
//   (1) The full (n_swaps + 1) × n_knots Jacobian from the adjoint — 1,001 reverse passes with
//       out_bar = e_o, run as 16 batched calls of up to 64 lanes with the same state in every lane
//       and the seed e_{first + b} in lane b (a lane of a batched run is bitwise the B = 1 run of
//       that lane, D31; spot-checked below for a few ordinals) — against jacobian_forward, the 12
//       passes of price_book<Dual<1>> along e_k (fixtures/m1_tangent.hpp), at the record point and
//       at the first 8 draws of the M2 state ball: |adj − dual| <= max(1e-12·|dual|, 1e-14) for
//       every entry.
//   (2) Transpose consistency: the adjoint of a random seed out_bar (uniform on [−1, 1]^1001,
//       Philox sub-stream 600000 + state) equals out_barᵀ·J with J the Dual Jacobian, per knot
//       |adj − ref| <= max(1e-12·|ref|, 1e-14), at the same 9 states. The reference dot products
//       are compensated (Neumaier) sums so that their own rounding stays out of the comparison.
//   (3) The dot-product identity <out_bar, J·u> = <Jᵀ·out_bar, u>: J·u from one Dual<1> pass along
//       a random direction u (sub-stream 610000 + state), Jᵀ·out_bar from the adjoint, 1e-12.
//
// The two sides share nothing below the templated maths: the adjoint is the reverse of the domain
// IR inferred from the recording (pulls in D31's fixed orders), the Dual is the same maths
// instantiated on a tangent scalar (tangent rules operation for operation), so their roundings
// differ and the gate is a tolerance. For the record the test prints the worst relative error,
// the worst error against the derivative's leg scale |d fixed_i/dz_k| + |d float_i/dz_k| (Σ_i |J_ik|
// for the book row) — the analogue of D26 for a derivative that is a difference of leg
// derivatives — the ulp distance, the counts of bitwise-equal, exactly-zero and ill-conditioned
// entries, and the book row's cancellation Σ_i |J_ik| / |J_book,k|. Release flags in this TU (the
// Dual may contract); the adjoint kernels are pinned E0 in their own TU (D25).
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_differential.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/m1_tangent.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/tape/tape.hpp"
#include "epykos/verify/differential.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;
namespace verify = epykos::verify;
using epykos::Dual;

namespace {

constexpr int n_knots = fixtures::n_knots;
constexpr int n_out = fixtures::n_swaps + 1;
constexpr int book_ordinal = fixtures::n_swaps;
constexpr int n_ball = 8;             // ball draws 0..7 after the record point
constexpr int n_states = 1 + n_ball;  // state 0 = the record point, s = 1 + draw
constexpr double rel_tol = 1e-12;
constexpr double abs_floor = 1e-14;
constexpr double well_posed = 1e-2;  // D26's threshold for the literal bound, applied to a derivative
constexpr std::uint64_t seed_stream_base = 600000;       // out_bar of state s: sub-stream base + s
constexpr std::uint64_t direction_stream_base = 610000;  // u of state s: sub-stream base + s
constexpr int max_reported_failures = 24;                // per state; every failure is counted

std::size_t idx(int o, int k) { return static_cast<std::size_t>(o) * static_cast<std::size_t>(n_knots) + static_cast<std::size_t>(k); }

std::string state_name(int s) { return s == 0 ? "record point" : "ball draw " + std::to_string(s - 1); }

// Compensated summation (Neumaier): the error of the sum itself is O(eps) of the result, not of
// the terms, so a reference dot product carries only the rounding of its products.
struct Neumaier {
  double sum = 0.0;
  double comp = 0.0;
  double abs_sum = 0.0;  // Σ |terms|: the scale of the cancellation
  void add(double x) {
    const double t = sum + x;
    if (std::fabs(sum) >= std::fabs(x)) {
      comp += (sum - t) + x;
    } else {
      comp += (x - t) + sum;
    }
    sum = t;
    abs_sum += std::fabs(x);
  }
  double value() const { return sum + comp; }
};

struct Fixture {
  fixtures::Book book;
  epykos::Tape tape;
  ir::Program program;
  verify::StateBall ball;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.tape = fixtures::record_m1(x.book);
    x.program = ir::infer(x.tape);
    verify::BallOptions opts;  // WORKLOADS.md §M2: ρ = 0.005, sub-stream 200000 + r; computed in an E0 TU (D30)
    opts.draws = n_ball;
    x.ball = fixtures::m1_state_ball(x.book, opts);
    return x;
  }();
  return f;
}

// Built after the fixture is in its final place: the Adjoint keeps a reference to the program.
const adjoint::Adjoint& the_adjoint() {
  static const adjoint::Adjoint ad(fixture().program);
  return ad;
}

std::vector<double> state_of(const Fixture& f, int s) {
  if (s == 0) return std::vector<double>(f.book.z0.begin(), f.book.z0.end());
  const double* z = f.ball.state(s - 1);
  return std::vector<double>(z, z + n_knots);
}

// The full Jacobian from the adjoint: row o is state_bar for the seed out_bar = e_o. Batched:
// calls of up to max_batch() lanes, the same state in every lane, e_{first + b} in lane b. The
// forward outputs are taken from lane 0 of the first call.
struct AdjointJacobian {
  std::vector<double> value;  // n_out
  std::vector<double> jac;    // n_out × n_knots, row-major
  int calls = 0;
  double seconds = 0.0;
};

AdjointJacobian adjoint_jacobian(const adjoint::Adjoint& ad, const double* z) {
  const auto t0 = std::chrono::steady_clock::now();
  AdjointJacobian j;
  const int max_b = ad.max_batch();
  j.value.assign(static_cast<std::size_t>(n_out), 0.0);
  j.jac.assign(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(n_knots), 0.0);
  std::vector<double> state, out_bar, out, state_bar;
  for (int first = 0; first < n_out; first += max_b) {
    const int B = std::min(max_b, n_out - first);
    const auto Bs = static_cast<std::size_t>(B);
    state.assign(static_cast<std::size_t>(n_knots) * Bs, 0.0);
    out_bar.assign(static_cast<std::size_t>(n_out) * Bs, 0.0);
    out.assign(static_cast<std::size_t>(n_out) * Bs, 0.0);
    state_bar.assign(static_cast<std::size_t>(n_knots) * Bs, 0.0);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_knots); ++k) {
      for (std::size_t b = 0; b < Bs; ++b) state[k * Bs + b] = z[k];
    }
    for (std::size_t b = 0; b < Bs; ++b) out_bar[(static_cast<std::size_t>(first) + b) * Bs + b] = 1.0;
    ad.run(state.data(), B, out_bar.data(), out.data(), state_bar.data());
    ++j.calls;
    if (first == 0) {
      for (int o = 0; o < n_out; ++o) j.value[static_cast<std::size_t>(o)] = out[static_cast<std::size_t>(o) * Bs];
    }
    for (std::size_t b = 0; b < Bs; ++b) {
      for (int k = 0; k < n_knots; ++k) j.jac[idx(first + static_cast<int>(b), k)] = state_bar[static_cast<std::size_t>(k) * Bs + b];
    }
  }
  j.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return j;
}

// The scale of the terms behind each derivative (D26's form for a derivative): per swap and knot
// |d fixed_i/dz_k| + |d float_i/dz_k| by one Dual<n_knots> pass per leg, and for the book row
// Σ_i |J_ik| over the Dual Jacobian's swap rows (the terms of the book fold). Also the leg values
// |fixed_i| + |float_i| (Σ_i over the book) for the forward outputs.
struct Scales {
  std::vector<double> deriv;  // n_out × n_knots
  std::vector<double> value;  // n_out
};

Scales scales_of(const fixtures::Book& b, const double* z, const fixtures::Jacobian& dual) {
  using Wide = Dual<n_knots>;
  std::vector<Wide> zd(static_cast<std::size_t>(n_knots));
  for (int k = 0; k < n_knots; ++k) zd[static_cast<std::size_t>(k)] = Wide::variable(z[k], k);
  Scales s;
  s.deriv.assign(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(n_knots), 0.0);
  s.value.assign(static_cast<std::size_t>(n_out), 0.0);
  double sum_abs_pv = 0.0;
  for (int i = 0; i < b.n_swaps; ++i) {
    const Wide fixed = fixtures::fixed_leg_pv<Wide>(b, i, zd.data());
    const Wide flt = fixtures::float_leg_pv<Wide>(b, i, zd.data());
    s.value[static_cast<std::size_t>(i)] = std::fabs(fixed.v) + std::fabs(flt.v);
    sum_abs_pv += std::fabs(fixed.v - flt.v);
    for (int k = 0; k < n_knots; ++k) {
      const auto ks = static_cast<std::size_t>(k);
      s.deriv[idx(i, k)] = std::fabs(fixed.d[ks]) + std::fabs(flt.d[ks]);
      s.deriv[idx(book_ordinal, k)] += std::fabs(dual.at(i, k));
    }
  }
  s.value[static_cast<std::size_t>(book_ordinal)] = sum_abs_pv;
  return s;
}

// Everything the tests need at one state, computed once.
struct StateData {
  std::vector<double> z;
  AdjointJacobian adj;
  fixtures::Jacobian dual;
  Scales scales;
};

const StateData& state_data(int s) {
  static std::unique_ptr<StateData> cache[n_states];
  if (!cache[s]) {
    const Fixture& f = fixture();
    auto d = std::make_unique<StateData>();
    d->z = state_of(f, s);
    d->adj = adjoint_jacobian(the_adjoint(), d->z.data());
    d->dual = fixtures::jacobian_forward(f.book, d->z.data());
    d->scales = scales_of(f.book, d->z.data(), d->dual);
    cache[s] = std::move(d);
  }
  return *cache[s];
}

struct Stats {
  std::size_t compared = 0;
  std::size_t failures = 0;         // |Δ| > max(rel·|dual|, floor)
  std::size_t exact_zeros = 0;      // 0 on both sides (no dependence)
  std::size_t bitwise_equal = 0;    // the same bits
  std::size_t ill_conditioned = 0;  // |dual| < well_posed × scale (D26's threshold): the literal bound is stiffest here
  double worst_rel = 0.0;           // |Δ| / max(|dual|, floor/rel)
  double worst_scaled = 0.0;        // |Δ| / scale
  double worst_ulps = 0.0;          // ulp distance at max(|adj|, |dual|)
  double worst_ill_rel = 0.0;       // worst_rel over the ill-conditioned entries only
  int worst_s = -1, worst_o = -1, worst_k = -1;
  std::size_t reported = 0;
};

void compare_entry(Stats& st, int s, int o, int k, double a, double d, double scale) {
  const double diff = std::fabs(a - d);
  const double tol = std::max(rel_tol * std::fabs(d), abs_floor);
  const double rel = diff / std::max(std::fabs(d), abs_floor / rel_tol);
  const bool ill = std::fabs(d) < well_posed * scale;
  ++st.compared;
  if (a == 0.0 && d == 0.0) ++st.exact_zeros;
  if (std::memcmp(&a, &d, sizeof a) == 0) ++st.bitwise_equal;
  if (ill) {
    ++st.ill_conditioned;
    st.worst_ill_rel = std::max(st.worst_ill_rel, rel);
  }
  if (rel > st.worst_rel) {
    st.worst_rel = rel;
    st.worst_s = s;
    st.worst_o = o;
    st.worst_k = k;
  }
  if (scale > 0.0) st.worst_scaled = std::max(st.worst_scaled, diff / scale);
  st.worst_ulps = std::max(st.worst_ulps, verify::ulp_distance(a, d));
  if (diff <= tol) return;
  ++st.failures;
  if (st.reported >= max_reported_failures) return;
  ++st.reported;
  ADD_FAILURE() << state_name(s) << (o >= 0 ? ", output " + std::to_string(o) : std::string()) << ", knot " << k << ": adjoint "
                << std::setprecision(17) << a << " vs dual " << d << " (|diff| " << diff << ", tol " << tol << ", relative " << rel << ", " << verify::ulp_distance(a, d)
                << " ulps, scale " << scale << ", |diff|/scale " << (scale > 0.0 ? diff / scale : 0.0)
                << (ill ? ", ill-conditioned: |dual| < 1e-2 x scale" : "") << ")";
}

void report(const Stats& st, const std::string& what) {
  std::cout << "[  vs dual ] " << what << ": " << st.compared << " entries, " << st.bitwise_equal << " bitwise equal, " << st.exact_zeros
            << " exact zeros on both sides, " << st.ill_conditioned << " ill-conditioned (|dual| < 1e-2 x scale; worst relative there "
            << st.worst_ill_rel << "); worst |adj - dual| / |dual| = " << st.worst_rel;
  if (st.worst_s >= 0) {
    std::cout << " (" << state_name(st.worst_s);
    if (st.worst_o >= 0) std::cout << ", output " << st.worst_o;
    std::cout << ", knot " << st.worst_k << ")";
  }
  std::cout << ", worst |adj - dual| / scale = " << st.worst_scaled << ", worst ulp distance " << st.worst_ulps << "; " << st.failures
            << " outside 1e-12 rel / 1e-14 abs\n";
}

// out_bar of state s: uniform on [−1, 1] per output from sub-stream seed_stream_base + s.
std::vector<double> random_seed(int s) {
  std::vector<double> ob(static_cast<std::size_t>(n_out));
  epykos::rng::Philox g(fixtures::default_seed, seed_stream_base + static_cast<std::uint64_t>(s));
  for (double& x : ob) x = g.uniform_range(-1.0, 1.0);
  return ob;
}

// u of state s: uniform on [−1, 1] per knot from sub-stream direction_stream_base + s.
std::vector<double> random_direction(int s) {
  std::vector<double> u(static_cast<std::size_t>(n_knots));
  epykos::rng::Philox g(fixtures::default_seed, direction_stream_base + static_cast<std::uint64_t>(s));
  for (double& x : u) x = g.uniform_range(-1.0, 1.0);
  return u;
}

}  // namespace

// ---- (1) the Jacobian gate --------------------------------------------------------------------

TEST(M1AdjointVsDual, JacobianMatchesForwardModeAtRecordPointAndBallStates) {
  Stats all;
  Stats book_rows;
  double worst_cancellation = 0.0;  // Σ_i |J_ik| / |J_book,k| over states and knots
  double worst_value_scaled = 0.0;  // forward outputs vs the Dual's value channel, against the leg scale
  std::size_t value_bitwise = 0;
  for (int s = 0; s < n_states; ++s) {
    const StateData& d = state_data(s);
    ASSERT_EQ(d.dual.n_outputs, n_out);
    ASSERT_EQ(d.dual.n_inputs, n_knots);
    ASSERT_EQ(d.adj.jac.size(), d.dual.jac.size());
    Stats st;
    for (int o = 0; o < n_out; ++o) {
      for (int k = 0; k < n_knots; ++k) {
        compare_entry(st, s, o, k, d.adj.jac[idx(o, k)], d.dual.at(o, k), d.scales.deriv[idx(o, k)]);
        if (o == book_ordinal) {
          compare_entry(book_rows, s, o, k, d.adj.jac[idx(o, k)], d.dual.at(o, k), d.scales.deriv[idx(o, k)]);
          const double jb = std::fabs(d.dual.at(o, k));
          if (jb > 0.0) worst_cancellation = std::max(worst_cancellation, d.scales.deriv[idx(o, k)] / jb);
        }
      }
    }
    // The forward outputs of the adjoint's own pass vs the Dual's value channel: bitwise under the
    // reference preset (both E0); under release flags the Dual TU may contract, so D26's bound.
    for (int o = 0; o < n_out; ++o) {
      const auto os = static_cast<std::size_t>(o);
      const double a = d.adj.value[os];
      const double v = d.dual.value[os];
      if (std::memcmp(&a, &v, sizeof a) == 0) ++value_bitwise;
      const double scale = d.scales.value[os];
      worst_value_scaled = std::max(worst_value_scaled, std::fabs(a - v) / scale);
      EXPECT_LE(std::fabs(a - v), 1e-12 * scale) << state_name(s) << ", output " << o << ": adjoint forward " << a << " vs dual value " << v;
    }
    std::cout << "[  state   ] " << state_name(s) << ": adjoint Jacobian in " << d.adj.calls << " batched calls (" << std::fixed
              << std::setprecision(3) << d.adj.seconds << " s)" << std::defaultfloat << ", worst |adj - dual| / |dual| " << st.worst_rel
              << " (output " << st.worst_o << ", knot " << st.worst_k << "), worst / scale " << st.worst_scaled << ", " << st.failures
              << " failures\n";
    all.compared += st.compared;
    all.failures += st.failures;
    all.exact_zeros += st.exact_zeros;
    all.bitwise_equal += st.bitwise_equal;
    all.ill_conditioned += st.ill_conditioned;
    all.worst_scaled = std::max(all.worst_scaled, st.worst_scaled);
    all.worst_ulps = std::max(all.worst_ulps, st.worst_ulps);
    all.worst_ill_rel = std::max(all.worst_ill_rel, st.worst_ill_rel);
    if (st.worst_rel > all.worst_rel) {
      all.worst_rel = st.worst_rel;
      all.worst_s = st.worst_s;
      all.worst_o = st.worst_o;
      all.worst_k = st.worst_k;
    }
  }
  report(all, "full Jacobian, 9 states");
  report(book_rows, "book rows only");
  std::cout << "[  book    ] worst cancellation of the book row, sum_i |J_ik| / |J_book,k| = " << worst_cancellation << '\n';
  std::cout << "[  values  ] adjoint forward outputs vs the Dual value channel: " << value_bitwise << " of " << n_out * n_states
            << " bitwise, worst |diff| / (|fixed| + |float|) = " << worst_value_scaled << '\n';
  EXPECT_EQ(all.failures, 0u) << "Jacobian entries outside 1e-12 rel / 1e-14 abs";
  EXPECT_LE(all.worst_rel, rel_tol);
}

// The batched trick relies on D31: a lane of a batched run is bitwise the B = 1 run of that lane.
TEST(M1AdjointVsDual, BatchedJacobianRowsAreTheSingleSeedRuns) {
  const adjoint::Adjoint& ad = the_adjoint();
  std::vector<double> out_bar(static_cast<std::size_t>(n_out), 0.0), state_bar(static_cast<std::size_t>(n_knots)),
      out(static_cast<std::size_t>(n_out));
  for (int s : {0, 3}) {
    const StateData& d = state_data(s);
    for (int o : {0, 63, 64, 199, 200, 511, 960, 999, book_ordinal}) {
      const auto os = static_cast<std::size_t>(o);
      out_bar[os] = 1.0;
      ad.run(d.z.data(), 1, out_bar.data(), out.data(), state_bar.data());
      out_bar[os] = 0.0;
      for (int k = 0; k < n_knots; ++k) {
        const double a = d.adj.jac[idx(o, k)];
        const double b = state_bar[static_cast<std::size_t>(k)];
        EXPECT_EQ(std::memcmp(&a, &b, sizeof a), 0) << state_name(s) << ", output " << o << ", knot " << k << ": batched " << a << " vs B=1 " << b;
      }
      for (int p = 0; p < n_out; ++p) {
        const auto ps = static_cast<std::size_t>(p);
        EXPECT_EQ(std::memcmp(&out[ps], &d.adj.value[ps], sizeof(double)), 0) << state_name(s) << ", forward output " << p;
      }
    }
  }
  EXPECT_EQ(state_data(0).adj.calls, (n_out + ad.max_batch() - 1) / ad.max_batch());
}

// ---- (2) transpose consistency ----------------------------------------------------------------

TEST(M1AdjointVsDual, AdjointOfRandomSeedIsSeedTransposeTimesDualJacobian) {
  const adjoint::Adjoint& ad = the_adjoint();
  Stats st;
  double worst_cancellation = 0.0;  // Σ_o |out_bar_o J_ok| / |Σ_o out_bar_o J_ok|
  std::vector<double> state_bar(static_cast<std::size_t>(n_knots));
  for (int s = 0; s < n_states; ++s) {
    const StateData& d = state_data(s);
    const std::vector<double> out_bar = random_seed(s);
    ad.run(d.z.data(), 1, out_bar.data(), nullptr, state_bar.data());
    for (int k = 0; k < n_knots; ++k) {
      Neumaier ref;
      for (int o = 0; o < n_out; ++o) ref.add(out_bar[static_cast<std::size_t>(o)] * d.dual.at(o, k));
      const double r = ref.value();
      if (r != 0.0) worst_cancellation = std::max(worst_cancellation, ref.abs_sum / std::fabs(r));
      compare_entry(st, s, -1, k, state_bar[static_cast<std::size_t>(k)], r, ref.abs_sum);
    }
  }
  report(st, "J^T out_bar for a random out_bar, 9 states x 12 knots (scale = sum_o |out_bar_o J_ok|)");
  std::cout << "[  cancel  ] worst sum_o |out_bar_o J_ok| / |sum_o out_bar_o J_ok| = " << worst_cancellation << '\n';
  EXPECT_EQ(st.failures, 0u) << "J^T out_bar components outside 1e-12 rel / 1e-14 abs";
  EXPECT_LE(st.worst_rel, rel_tol);
}

// ---- (3) the dot-product identity -------------------------------------------------------------

TEST(M1AdjointVsDual, DotProductIdentityBetweenTangentAndAdjoint) {
  const Fixture& f = fixture();
  const adjoint::Adjoint& ad = the_adjoint();
  double worst_rel = 0.0;
  double worst_scaled = 0.0;
  std::vector<double> state_bar(static_cast<std::size_t>(n_knots));
  for (int s = 0; s < n_states; ++s) {
    const StateData& d = state_data(s);
    const std::vector<double> out_bar = random_seed(s);
    const std::vector<double> u = random_direction(s);
    // <out_bar, J u>: J u by one Dual<1> pass along u.
    const fixtures::Tangent t = fixtures::tangent(f.book, d.z.data(), u.data());
    Neumaier lhs;
    for (int o = 0; o < n_out; ++o) lhs.add(out_bar[static_cast<std::size_t>(o)] * t.tangent[static_cast<std::size_t>(o)]);
    // <J^T out_bar, u>: J^T out_bar by the adjoint.
    ad.run(d.z.data(), 1, out_bar.data(), nullptr, state_bar.data());
    Neumaier rhs;
    for (int k = 0; k < n_knots; ++k) rhs.add(state_bar[static_cast<std::size_t>(k)] * u[static_cast<std::size_t>(k)]);
    const double l = lhs.value();
    const double r = rhs.value();
    const double diff = std::fabs(l - r);
    const double m = std::max(std::fabs(l), std::fabs(r));
    const double tol = std::max(rel_tol * m, abs_floor);
    const double scale = std::max(lhs.abs_sum, rhs.abs_sum);
    worst_rel = std::max(worst_rel, diff / std::max(m, abs_floor / rel_tol));
    worst_scaled = std::max(worst_scaled, diff / scale);
    EXPECT_LE(diff, tol) << state_name(s) << ": <out_bar, J u> = " << std::setprecision(17) << l << " vs <J^T out_bar, u> = " << r
                         << " (|diff| " << diff << ", sum of |terms| " << lhs.abs_sum << " / " << rhs.abs_sum << ")";
    std::cout << "[  dot     ] " << state_name(s) << ": <out_bar, J u> = " << std::setprecision(17) << l << ", <J^T out_bar, u> = " << r
              << std::setprecision(6) << ", relative " << diff / std::max(m, abs_floor / rel_tol) << ", / sum|terms| " << diff / scale << '\n';
  }
  std::cout << "[  dot     ] worst relative " << worst_rel << ", worst / sum of |terms| " << worst_scaled << '\n';
  EXPECT_LE(worst_rel, rel_tol);
}
