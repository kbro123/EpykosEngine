// M3/G2 gate: the mechanical adjoint of the instrument sample (fixtures/instrument_sample.hpp:
// every Stage A trade type, the compounded coupons as scans) against forward mode — the full
// Jacobian from adjoint::Adjoint (one lane per output) vs one pass of the same templated maths
// on Dual<44> at 1e-12 relative to the row's scale (D26: the leg scale of a swap PV), at the
// record point and on ball states; and vs central finite differences at 1e-6. Release flags:
// tolerance gates by nature. A gate of scripts/mutation_test.sh (the name matches
// "_vs_dual_test$").
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/instrument_sample.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/tape/tape.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;
using epykos::Dual;

namespace {

constexpr int N = fixtures::sample_curves * fixtures::sample_knots;
constexpr double dual_rel = 1e-12;
constexpr double fd_h = 1e-6;
constexpr double fd_rel = 1e-6;

struct Jacobian {
  std::vector<double> value;
  std::vector<std::vector<double>> jac;   // [o][k]
};

Jacobian jacobian_by_dual(const fixtures::InstrumentSample& s, const std::vector<double>& z) {
  using D = Dual<N>;
  std::vector<D> in(static_cast<std::size_t>(N));
  for (int k = 0; k < N; ++k) in[static_cast<std::size_t>(k)] = D::variable(z[static_cast<std::size_t>(k)], k);
  std::vector<D> out(static_cast<std::size_t>(s.n_outputs()));
  fixtures::price_sample<D>(s, in.data(), out.data());
  Jacobian j;
  j.value.resize(out.size());
  j.jac.assign(out.size(), std::vector<double>(static_cast<std::size_t>(N), 0.0));
  for (std::size_t o = 0; o < out.size(); ++o) {
    j.value[o] = out[o].v;
    for (int k = 0; k < N; ++k) j.jac[o][static_cast<std::size_t>(k)] = out[o].d[static_cast<std::size_t>(k)];
  }
  return j;
}

std::vector<std::vector<double>> adjoint_jacobian(const adjoint::Adjoint& ad, const std::vector<double>& z, std::vector<double>* out) {
  const int n_out = ad.n_outputs(), n_in = ad.n_inputs();
  std::vector<std::vector<double>> adj(static_cast<std::size_t>(n_out), std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
  out->assign(static_cast<std::size_t>(n_out), 0.0);
  for (int o0 = 0; o0 < n_out; o0 += ad.max_batch()) {
    const int B = std::min(ad.max_batch(), n_out - o0);
    const std::size_t Bs = static_cast<std::size_t>(B);
    std::vector<double> st(static_cast<std::size_t>(n_in) * Bs), ob(static_cast<std::size_t>(n_out) * Bs, 0.0);
    std::vector<double> fwd(static_cast<std::size_t>(n_out) * Bs), sb(static_cast<std::size_t>(n_in) * Bs);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) {
      for (std::size_t b = 0; b < Bs; ++b) st[k * Bs + b] = z[k];
    }
    for (std::size_t b = 0; b < Bs; ++b) ob[(static_cast<std::size_t>(o0) + b) * Bs + b] = 1.0;
    ad.run(st.data(), B, ob.data(), fwd.data(), sb.data());
    for (std::size_t b = 0; b < Bs; ++b) {
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) adj[static_cast<std::size_t>(o0) + b][k] = sb[k * Bs + b];
    }
    for (std::size_t o = 0; o < static_cast<std::size_t>(n_out); ++o) (*out)[o] = fwd[o * Bs];
  }
  return adj;
}

std::vector<std::vector<double>> fd_jacobian(const fixtures::InstrumentSample& s, const std::vector<double>& z) {
  const std::size_t n_out = static_cast<std::size_t>(s.n_outputs());
  std::vector<std::vector<double>> fd(n_out, std::vector<double>(static_cast<std::size_t>(N), 0.0));
  std::vector<double> fp(n_out), fm(n_out);
  for (int k = 0; k < N; ++k) {
    std::vector<double> zp = z, zm = z;
    zp[static_cast<std::size_t>(k)] += fd_h;
    zm[static_cast<std::size_t>(k)] -= fd_h;
    fixtures::price_sample<double>(s, zp.data(), fp.data());
    fixtures::price_sample<double>(s, zm.data(), fm.data());
    for (std::size_t o = 0; o < n_out; ++o) fd[o][static_cast<std::size_t>(k)] = (fp[o] - fm[o]) / (2.0 * fd_h);
  }
  return fd;
}

}  // namespace

TEST(SampleAdjointVsDual, JacobianMatchesForwardModeAndFiniteDifferences) {
  const fixtures::InstrumentSample s = fixtures::make_instrument_sample();
  const ir::Program program = ir::infer(fixtures::record_sample(s));
  ASSERT_FALSE(ir::scan_domains(program).empty());
  adjoint::Adjoint ad(program);
  ASSERT_EQ(ad.n_inputs(), N);
  ASSERT_EQ(ad.n_outputs(), s.n_outputs());
  std::vector<std::vector<double>> states;
  states.push_back(s.z0);
  for (const std::vector<double>& z : fixtures::sample_states(s, 3)) states.push_back(z);
  std::size_t compared = 0, dual_failures = 0, fd_failures = 0, value_mismatches = 0;
  double worst_dual = 0.0, worst_fd = 0.0;
  for (std::size_t st = 0; st < states.size(); ++st) {
    const std::vector<double>& z = states[st];
    const std::string where = st == 0 ? "record point" : "state " + std::to_string(st);
    const Jacobian j = jacobian_by_dual(s, z);
    const std::vector<double> scale = fixtures::sample_scales(s, z.data());
    std::vector<double> out;
    const std::vector<std::vector<double>> adj = adjoint_jacobian(ad, z, &out);
    const std::vector<std::vector<double>> fd = fd_jacobian(s, z);
    for (std::size_t o = 0; o < adj.size(); ++o) {
      if (std::fabs(out[o] - j.value[o]) > 1e-12 * std::max(std::fabs(j.value[o]), scale[o])) {
        if (value_mismatches < 5) ADD_FAILURE() << where << ", output " << o << ": forward " << out[o] << " vs Dual value " << j.value[o];
        ++value_mismatches;
      }
      double row_scale = 0.0;
      for (std::size_t k = 0; k < adj[o].size(); ++k) row_scale = std::max(row_scale, std::fabs(j.jac[o][k]));
      row_scale = std::max(row_scale, scale[o]);
      for (std::size_t k = 0; k < adj[o].size(); ++k) {
        const double a = adj[o][k], d = j.jac[o][k], f = fd[o][k];
        ++compared;
        const double ed = std::fabs(a - d) / row_scale;
        worst_dual = std::max(worst_dual, ed);
        if (ed > dual_rel) {
          if (dual_failures < 5) ADD_FAILURE() << where << ", output " << o << " (" << (o < static_cast<std::size_t>(s.n_trades()) ? s.instruments[o].id : "book") << "), input " << k << ": adjoint " << a << " vs dual " << d;
          ++dual_failures;
        }
        const double ef = std::fabs(a - f) / row_scale;
        worst_fd = std::max(worst_fd, ef);
        if (ef > fd_rel) {
          if (fd_failures < 5) ADD_FAILURE() << where << ", output " << o << ", input " << k << ": adjoint " << a << " vs fd " << f;
          ++fd_failures;
        }
      }
    }
  }
  std::cout << "[  dual    ] instrument sample: " << compared << " Jacobian entries over " << states.size() << " states, worst |adj - dual| / scale = "
            << worst_dual << " (gate 1e-12), worst |adj - fd| / scale = " << worst_fd << " (gate 1e-6)\n";
  EXPECT_EQ(value_mismatches, 0u);
  EXPECT_EQ(dual_failures, 0u) << "adjoint vs Dual";
  EXPECT_EQ(fd_failures, 0u) << "adjoint vs finite differences";
}
