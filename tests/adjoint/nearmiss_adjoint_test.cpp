// M2/Q4b gate: the adjoint on the near-miss shapes fixture (fixtures/nearmiss_shapes.hpp) — every
// op the recorder produces, with a constant in every operand position, select with constant and
// swapped arms, max / min / abs, recip and 1/x, sums and affine chains — against the same maths
// instantiated on Dual<6> (the full Jacobian in one forward pass, 1e-12) and against central
// finite differences of the double instantiation (1e-6), on the raw recording and after the
// standard passes, at the record point and on a ball of states; plus linearity in the seed.
//
// The M1 book has no select, recip, fma, log or sqrt, so its adjoint gates (m1_adjoint_test.cpp,
// m1_adjoint_vs_dual_test.cpp) cannot exercise those rules: this gate is what catches a defect in
// them (D32: a survivor of the mutation harness is closed by a generic gate, never by a
// mutant-specific test). It is a gate of scripts/mutation_test.sh (the *_adjoint_test name).
//
// Tolerances. Dual: |adj − dual| <= 1e-12·max(|dual|, 1) per Jacobian entry (the shapes are O(1);
// both sides apply the same local rules in different orders). FD: |adj − fd| <= max(1e-6·|fd|,
// 1e-7) with h = 1e-6, asserted only where the central difference is valid: a select / max / min /
// abs whose predicate flips inside [x − h, x + h] straddles a kink and the difference is not a
// derivative there; those entries are recognised by |fd − dual| > the FD tolerance and counted, not
// asserted (the Dual comparison covers them exactly). Release flags: tolerance-based by nature.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/nearmiss_shapes.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;
using epykos::Dual;
using epykos::Tape;

namespace {

constexpr int n_in = fixtures::nearmiss_inputs;
constexpr int n_states = 16;
constexpr double dual_rel = 1e-12;
constexpr double fd_h = 1e-6;
constexpr double fd_rel = 1e-6;
constexpr double fd_floor = 1e-7;

struct Fixture {
  Tape raw;
  Tape passed;
  ir::Program program_raw;
  ir::Program program_passed;
  std::vector<std::vector<double>> states;  // record point, then draws
  int n_out = 0;
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.raw = fixtures::record_nearmiss();
    x.passed = x.raw;
    epykos::standard_passes(x.passed);
    x.program_raw = ir::infer(x.raw);
    x.program_passed = ir::infer(x.passed);
    x.states = fixtures::nearmiss_states(n_states);
    x.n_out = static_cast<int>(x.raw.num_outputs());
    return x;
  }();
  return f;
}

// The Jacobian by one pass of Dual<n_in>: jac[o][k] = d out_o / d in_k, and the values.
struct Jacobian {
  std::vector<double> value;
  std::vector<std::vector<double>> jac;  // [o][k]
};

Jacobian dual_jacobian(const std::vector<double>& state) {
  using D = Dual<n_in>;
  std::vector<D> in(static_cast<std::size_t>(n_in));
  for (int k = 0; k < n_in; ++k) in[static_cast<std::size_t>(k)] = D::variable(state[static_cast<std::size_t>(k)], k);
  const std::vector<D> out = fixtures::nearmiss_evaluate<D>(in.data());
  Jacobian j;
  j.value.resize(out.size());
  j.jac.assign(out.size(), std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
  for (std::size_t o = 0; o < out.size(); ++o) {
    j.value[o] = out[o].v;
    for (int k = 0; k < n_in; ++k) j.jac[o][static_cast<std::size_t>(k)] = out[o].d[static_cast<std::size_t>(k)];
  }
  return j;
}

// Central differences of every output with respect to every input: fd[o][k].
std::vector<std::vector<double>> fd_jacobian(const std::vector<double>& state, int n_out) {
  std::vector<std::vector<double>> fd(static_cast<std::size_t>(n_out), std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
  for (int k = 0; k < n_in; ++k) {
    std::vector<double> zp = state, zm = state;
    zp[static_cast<std::size_t>(k)] += fd_h;
    zm[static_cast<std::size_t>(k)] -= fd_h;
    const std::vector<double> fp = fixtures::nearmiss_oracle(zp);
    const std::vector<double> fm = fixtures::nearmiss_oracle(zm);
    for (std::size_t o = 0; o < fp.size(); ++o) fd[o][static_cast<std::size_t>(k)] = (fp[o] - fm[o]) / (2.0 * fd_h);
  }
  return fd;
}

// Every output's gradient by the adjoint at one state: one lane per output, out_bar = e_o, in
// batches of at most max_batch lanes. Returns adj[o][k]; `out` receives the forward outputs.
std::vector<std::vector<double>> adjoint_jacobian(const adjoint::Adjoint& ad, const std::vector<double>& state, std::vector<double>* out) {
  const int n_out = ad.n_outputs();
  std::vector<std::vector<double>> adj(static_cast<std::size_t>(n_out), std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
  out->assign(static_cast<std::size_t>(n_out), 0.0);
  for (int o0 = 0; o0 < n_out; o0 += ad.max_batch()) {
    const int B = std::min(ad.max_batch(), n_out - o0);
    const std::size_t Bs = static_cast<std::size_t>(B);
    std::vector<double> st(static_cast<std::size_t>(n_in) * Bs), ob(static_cast<std::size_t>(n_out) * Bs, 0.0);
    std::vector<double> fwd(static_cast<std::size_t>(n_out) * Bs), sb(static_cast<std::size_t>(n_in) * Bs);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) {
      for (std::size_t b = 0; b < Bs; ++b) st[k * Bs + b] = state[k];
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

struct Comparison {
  std::size_t compared = 0;
  std::size_t failures = 0;
  std::size_t kinks = 0;        // FD only: entries where the central difference straddles a kink
  double worst = 0.0;           // worst |a − b| / tolerance scale
};

void compare_dual(Comparison& c, const std::vector<std::vector<double>>& adj, const Jacobian& j, const std::string& where) {
  for (std::size_t o = 0; o < adj.size(); ++o) {
    for (std::size_t k = 0; k < adj[o].size(); ++k) {
      const double a = adj[o][k];
      const double d = j.jac[o][k];
      const double diff = std::fabs(a - d);
      const double scale = std::max(std::fabs(d), 1.0);
      ++c.compared;
      c.worst = std::max(c.worst, diff / scale);
      if (diff <= dual_rel * scale) continue;
      ++c.failures;
      if (c.failures <= 8) ADD_FAILURE() << where << ", output " << o << ", input " << k << ": adjoint " << a << " vs dual " << d << " (diff " << diff << ")";
    }
  }
}

void compare_fd(Comparison& c, const std::vector<std::vector<double>>& adj, const std::vector<std::vector<double>>& fd,
                const Jacobian& j, const std::string& where) {
  for (std::size_t o = 0; o < adj.size(); ++o) {
    for (std::size_t k = 0; k < adj[o].size(); ++k) {
      const double f = fd[o][k];
      const double tol = std::max(fd_rel * std::fabs(f), fd_floor);
      if (std::fabs(f - j.jac[o][k]) > tol) {  // the difference straddles a kink: not a derivative here
        ++c.kinks;
        continue;
      }
      const double a = adj[o][k];
      const double diff = std::fabs(a - f);
      ++c.compared;
      c.worst = std::max(c.worst, diff / tol);
      if (diff <= tol) continue;
      ++c.failures;
      if (c.failures <= 8) ADD_FAILURE() << where << ", output " << o << ", input " << k << ": adjoint " << a << " vs fd " << f << " (diff " << diff << ", tol " << tol << ")";
    }
  }
}

void check_program(const ir::Program& program, const char* what) {
  const Fixture& f = fixture();
  adjoint::Adjoint ad(program);
  ASSERT_EQ(ad.n_inputs(), n_in);
  ASSERT_EQ(ad.n_outputs(), f.n_out);
  Comparison dual, fd;
  std::size_t value_mismatches = 0;
  for (std::size_t s = 0; s < f.states.size(); ++s) {
    const std::string where = std::string(what) + (s == 0 ? ", record point" : ", state " + std::to_string(s));
    const Jacobian j = dual_jacobian(f.states[s]);
    std::vector<double> out;
    const std::vector<std::vector<double>> adj = adjoint_jacobian(ad, f.states[s], &out);
    for (std::size_t o = 0; o < out.size(); ++o) {
      if (std::fabs(out[o] - j.value[o]) > 1e-12 * std::max(std::fabs(j.value[o]), 1.0)) {
        if (value_mismatches < 5) ADD_FAILURE() << where << ", output " << o << ": forward " << out[o] << " vs the Dual value " << j.value[o];
        ++value_mismatches;
      }
    }
    compare_dual(dual, adj, j, where);
    compare_fd(fd, adj, fd_jacobian(f.states[s], f.n_out), j, where);
  }
  std::cout << "[  dual    ] " << what << ": " << dual.compared << " Jacobian entries over " << f.states.size()
            << " states, worst |adj - dual| / max(|dual|, 1) = " << dual.worst << ", " << dual.failures << " outside 1e-12\n";
  std::cout << "[  fd      ] " << what << ": " << fd.compared << " entries compared (" << fd.kinks
            << " straddle a kink and are covered by the Dual comparison), worst |adj - fd| / tol = " << fd.worst << ", "
            << fd.failures << " outside 1e-6 rel / 1e-7 abs\n";
  EXPECT_EQ(value_mismatches, 0u);
  EXPECT_EQ(dual.failures, 0u) << what << ": adjoint vs Dual";
  EXPECT_EQ(fd.failures, 0u) << what << ": adjoint vs finite differences";
  EXPECT_GT(fd.compared, fd.kinks * 4) << "too few valid finite differences to be a gate";
}

}  // namespace

TEST(NearmissAdjoint, RawRecordingMatchesTheDualJacobianAndFiniteDifferences) {
  const Fixture& f = fixture();
  check_program(f.program_raw, "raw");
}

TEST(NearmissAdjoint, AfterThePassesMatchesTheDualJacobianAndFiniteDifferences) {
  const Fixture& f = fixture();
  check_program(f.program_passed, "after passes");
}

TEST(NearmissAdjoint, AdjointIsLinearInTheSeedOnBothPrograms) {
  const Fixture& f = fixture();
  for (const ir::Program* program : {&f.program_raw, &f.program_passed}) {
    adjoint::Adjoint ad(*program);
    const std::size_t n_out = static_cast<std::size_t>(f.n_out);
    epykos::rng::Philox g(fixtures::nearmiss_seed, 420000);
    std::vector<double> ob1(n_out), ob2(n_out), ob12(n_out);
    for (std::size_t o = 0; o < n_out; ++o) {
      ob1[o] = g.uniform_range(-1.0, 1.0);
      ob2[o] = g.uniform_range(-1.0, 1.0);
      ob12[o] = ob1[o] + ob2[o];
    }
    for (std::size_t s = 0; s < f.states.size(); s += 5) {
      const std::vector<double>& z = f.states[s];
      std::vector<double> sb1(static_cast<std::size_t>(n_in)), sb2(static_cast<std::size_t>(n_in)), sb12(static_cast<std::size_t>(n_in));
      ad.run(z.data(), 1, ob1.data(), nullptr, sb1.data());
      ad.run(z.data(), 1, ob2.data(), nullptr, sb2.data());
      ad.run(z.data(), 1, ob12.data(), nullptr, sb12.data());
      // The seeded adjoint is also J^T applied to the seed: check it against the Dual Jacobian.
      const Jacobian j = dual_jacobian(z);
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) {
        const double scale = std::fabs(sb1[k]) + std::fabs(sb2[k]);
        EXPECT_LE(std::fabs(sb12[k] - (sb1[k] + sb2[k])), 1e-13 * std::max(scale, 1.0)) << "state " << s << ", input " << k;
        double jt = 0.0, jt_scale = 0.0;
        for (std::size_t o = 0; o < n_out; ++o) {
          jt += j.jac[o][k] * ob1[o];
          jt_scale += std::fabs(j.jac[o][k] * ob1[o]);
        }
        EXPECT_LE(std::fabs(sb1[k] - jt), 1e-12 * std::max(jt_scale, 1.0)) << "state " << s << ", input " << k << ": adjoint " << sb1[k] << " vs J^T seed " << jt;
      }
    }
  }
}
