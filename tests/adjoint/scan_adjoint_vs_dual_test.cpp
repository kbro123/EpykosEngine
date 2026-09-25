// M3/G3 gate: the reverse-scan adjoint (D41) against forward mode and finite differences on the
// scan fixtures — the RFR compounding book (fixtures/rfr_book.hpp) and the affine scan
// (fixtures/affine_scan.hpp, with and without the path as outputs). The full Jacobian from
// adjoint::Adjoint (one lane per output, out_bar = e_o) is compared with one pass of the same
// templated maths on Dual<n_inputs> at 1e-12 relative to max(|J|, scale) per entry, and with
// central finite differences of the double instantiation (h = 1e-6) at 1e-6 relative, at the
// record point and on a ball of states; plus linearity in the seed. Release flags: tolerance
// gates by nature.
//
// A gate of scripts/mutation_test.sh (the name matches "_vs_dual_test$"): the adjoint's scan
// mutant (the reverse scan run forwards) is caught here.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/fixtures/affine_scan.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/rfr_book.hpp"
#include "epykos/fixtures/rfr_price.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/tape/tape.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;
using epykos::Dual;
using epykos::Tape;

namespace {

constexpr double dual_rel = 1e-12;
constexpr double fd_h = 1e-6;
constexpr double fd_rel = 1e-6;

struct Jacobian {
  std::vector<double> value;             // n_out
  std::vector<std::vector<double>> jac;  // [o][k]
};

// A fixture the gate can drive: its program, its states, and the Dual / double evaluations.
struct Case {
  std::string name;
  int n_in = 0;
  int n_out = 0;
  ir::Program program;
  std::vector<std::vector<double>> states;
  std::function<Jacobian(const std::vector<double>&)> dual;
  std::function<std::vector<double>(const std::vector<double>&)> oracle;
  // The scale of an output at a state (|fixed| + |float| for a swap PV, D26; 1 otherwise).
  std::function<std::vector<double>(const std::vector<double>&)> scale;
};

template <int N, class Eval>
Jacobian jacobian_by_dual(const std::vector<double>& z, int n_out, Eval eval) {
  using D = Dual<N>;
  std::vector<D> in(static_cast<std::size_t>(N));
  for (int k = 0; k < N; ++k) in[static_cast<std::size_t>(k)] = D::variable(z[static_cast<std::size_t>(k)], k);
  std::vector<D> out(static_cast<std::size_t>(n_out));
  eval(in.data(), out.data());
  Jacobian j;
  j.value.resize(out.size());
  j.jac.assign(out.size(), std::vector<double>(static_cast<std::size_t>(N), 0.0));
  for (std::size_t o = 0; o < out.size(); ++o) {
    j.value[o] = out[o].v;
    for (int k = 0; k < N; ++k) j.jac[o][static_cast<std::size_t>(k)] = out[o].d[static_cast<std::size_t>(k)];
  }
  return j;
}

Case rfr_case() {
  static const fixtures::RfrBook book = fixtures::make_rfr_book();
  Case c;
  c.name = "RFR book";
  c.n_in = fixtures::n_knots;
  c.n_out = book.n_swaps + 1;
  c.program = ir::infer(fixtures::record_rfr(book));
  const fixtures::Batch batch = fixtures::make_m1_batch();
  c.states.push_back(std::vector<double>(book.z0.begin(), book.z0.end()));
  for (int b = 1; b < batch.n_states; b += 9) {  // 7 draws
    std::vector<double> z(static_cast<std::size_t>(fixtures::n_knots));
    batch.state(b, z.data());
    c.states.push_back(z);
  }
  c.dual = [](const std::vector<double>& z) {
    return jacobian_by_dual<fixtures::n_knots>(z, book.n_swaps + 1, [](const Dual<fixtures::n_knots>* in, Dual<fixtures::n_knots>* out) {
      fixtures::price_rfr_book(book, in, out, out + book.n_swaps);
    });
  };
  c.oracle = [](const std::vector<double>& z) {
    std::vector<double> out(static_cast<std::size_t>(book.n_swaps) + 1);
    fixtures::rfr_reference_state(book, z.data(), out.data());
    return out;
  };
  c.scale = [](const std::vector<double>& z) {
    std::vector<double> s(static_cast<std::size_t>(book.n_swaps) + 1, 0.0);
    double total = 0.0;
    for (int i = 0; i < book.n_swaps; ++i) {
      const double fixed = fixtures::rfr_fixed_leg_pv<double>(book, i, z.data());
      const double flt = fixtures::rfr_float_leg_pv<double>(book, i, z.data());
      s[static_cast<std::size_t>(i)] = std::fabs(fixed) + std::fabs(flt);
      total += std::fabs(fixed - flt);
    }
    s[static_cast<std::size_t>(book.n_swaps)] = total;
    return s;
  };
  return c;
}

Case affine_case(bool output_path) {
  static const fixtures::AffineScanFixture f = fixtures::make_affine_scan();
  constexpr int N = fixtures::affine_scan_paths + 3;
  Case c;
  c.name = std::string("affine scan (") + (output_path ? "path" : "finals") + ")";
  c.n_in = N;
  c.n_out = f.n_outputs(output_path);
  c.program = ir::infer(fixtures::record_affine_scan(f, output_path));
  c.states = fixtures::affine_scan_states(f, 8);
  c.dual = [output_path](const std::vector<double>& z) {
    return jacobian_by_dual<N>(z, f.n_outputs(output_path), [output_path](const Dual<N>* in, Dual<N>* out) {
      fixtures::affine_scan_evaluate(f, in, out, output_path);
    });
  };
  c.oracle = [output_path](const std::vector<double>& z) { return fixtures::affine_scan_oracle(f, z, output_path); };
  c.scale = [output_path](const std::vector<double>&) { return std::vector<double>(static_cast<std::size_t>(f.n_outputs(output_path)), 1.0); };
  return c;
}

const std::vector<Case>& cases() {
  static const std::vector<Case> all = [] {
    std::vector<Case> v;
    v.push_back(rfr_case());
    v.push_back(affine_case(false));
    v.push_back(affine_case(true));
    return v;
  }();
  return all;
}

// Every output's gradient by the adjoint, one lane per output; `out` receives the forward.
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

std::vector<std::vector<double>> fd_jacobian(const Case& c, const std::vector<double>& z) {
  std::vector<std::vector<double>> fd(static_cast<std::size_t>(c.n_out), std::vector<double>(static_cast<std::size_t>(c.n_in), 0.0));
  for (int k = 0; k < c.n_in; ++k) {
    std::vector<double> zp = z, zm = z;
    zp[static_cast<std::size_t>(k)] += fd_h;
    zm[static_cast<std::size_t>(k)] -= fd_h;
    const std::vector<double> fp = c.oracle(zp), fm = c.oracle(zm);
    for (std::size_t o = 0; o < fp.size(); ++o) fd[o][static_cast<std::size_t>(k)] = (fp[o] - fm[o]) / (2.0 * fd_h);
  }
  return fd;
}

}  // namespace

TEST(ScanAdjointVsDual, JacobianMatchesForwardModeAndFiniteDifferences) {
  for (const Case& c : cases()) {
    adjoint::Adjoint ad(c.program);
    ASSERT_EQ(ad.n_inputs(), c.n_in) << c.name;
    ASSERT_EQ(ad.n_outputs(), c.n_out) << c.name;
    ASSERT_FALSE(ir::scan_domains(c.program).empty()) << c.name;
    std::size_t compared = 0, dual_failures = 0, fd_failures = 0, value_mismatches = 0;
    double worst_dual = 0.0, worst_fd = 0.0;
    for (std::size_t s = 0; s < c.states.size(); ++s) {
      const std::vector<double>& z = c.states[s];
      const std::string where = c.name + (s == 0 ? ", record point" : ", state " + std::to_string(s));
      const Jacobian j = c.dual(z);
      const std::vector<double> scale = c.scale(z);
      std::vector<double> out;
      const std::vector<std::vector<double>> adj = adjoint_jacobian(ad, z, &out);
      const std::vector<std::vector<double>> fd = fd_jacobian(c, z);
      for (std::size_t o = 0; o < adj.size(); ++o) {
        if (std::fabs(out[o] - j.value[o]) > 1e-12 * std::max(std::fabs(j.value[o]), scale[o])) {
          if (value_mismatches < 5) ADD_FAILURE() << where << ", output " << o << ": forward " << out[o] << " vs Dual value " << j.value[o];
          ++value_mismatches;
        }
        // The Jacobian scale of output o: the largest |entry| of its row on the Dual side, or the
        // output's own scale when the row is tiny (the leg scale of a swap PV is the natural
        // unit of its sensitivities, D26).
        double row_scale = 0.0;
        for (std::size_t k = 0; k < adj[o].size(); ++k) row_scale = std::max(row_scale, std::fabs(j.jac[o][k]));
        row_scale = std::max(row_scale, scale[o]);
        for (std::size_t k = 0; k < adj[o].size(); ++k) {
          const double a = adj[o][k], d = j.jac[o][k], fdk = fd[o][k];
          ++compared;
          const double ed = std::fabs(a - d) / row_scale;
          worst_dual = std::max(worst_dual, ed);
          if (ed > dual_rel) {
            if (dual_failures < 5) ADD_FAILURE() << where << ", output " << o << ", input " << k << ": adjoint " << a << " vs dual " << d;
            ++dual_failures;
          }
          const double ef = std::fabs(a - fdk) / row_scale;
          worst_fd = std::max(worst_fd, ef);
          if (ef > fd_rel) {
            if (fd_failures < 5) ADD_FAILURE() << where << ", output " << o << ", input " << k << ": adjoint " << a << " vs fd " << fdk;
            ++fd_failures;
          }
        }
      }
    }
    std::cout << "[  dual    ] " << c.name << ": " << compared << " Jacobian entries over " << c.states.size()
              << " states, worst |adj - dual| / scale = " << worst_dual << " (gate 1e-12), worst |adj - fd| / scale = "
              << worst_fd << " (gate 1e-6)\n";
    EXPECT_EQ(value_mismatches, 0u) << c.name;
    EXPECT_EQ(dual_failures, 0u) << c.name << ": adjoint vs Dual";
    EXPECT_EQ(fd_failures, 0u) << c.name << ": adjoint vs finite differences";
  }
}

TEST(ScanAdjointVsDual, LinearInTheSeed) {
  for (const Case& c : cases()) {
    adjoint::Adjoint ad(c.program);
    const std::size_t n_out = static_cast<std::size_t>(c.n_out), n_in = static_cast<std::size_t>(c.n_in);
    epykos::rng::Philox g(fixtures::default_seed, 530000);
    std::vector<double> ob1(n_out), ob2(n_out), ob12(n_out);
    for (std::size_t o = 0; o < n_out; ++o) {
      ob1[o] = g.uniform_range(-1.0, 1.0);
      ob2[o] = g.uniform_range(-1.0, 1.0);
      ob12[o] = ob1[o] + ob2[o];
    }
    for (std::size_t s = 0; s < c.states.size(); s += 3) {
      const std::vector<double>& z = c.states[s];
      std::vector<double> sb1(n_in), sb2(n_in), sb12(n_in);
      ad.run(z.data(), 1, ob1.data(), nullptr, sb1.data());
      ad.run(z.data(), 1, ob2.data(), nullptr, sb2.data());
      ad.run(z.data(), 1, ob12.data(), nullptr, sb12.data());
      const Jacobian j = c.dual(z);
      const std::vector<double> out_scale = c.scale(z);
      for (std::size_t k = 0; k < n_in; ++k) {
        // The scale of input k's adjoint: the seed-weighted sum of the outputs' Jacobian scales
        // (the largest entry of the row, or the output's own scale, D26). A sensitivity that
        // cancels to rounding level (the compounded coupon telescopes: the knots inside its
        // accrual period have no net effect) is compared at the scale of what cancelled.
        double scale = 0.0, jt = 0.0;
        for (std::size_t o = 0; o < n_out; ++o) {
          double row = out_scale[o];
          for (std::size_t kk = 0; kk < n_in; ++kk) row = std::max(row, std::fabs(j.jac[o][kk]));
          scale += std::fabs(ob1[o]) * row;
          jt += j.jac[o][k] * ob1[o];
        }
        scale = std::max(scale, 1.0);
        EXPECT_LE(std::fabs(sb12[k] - (sb1[k] + sb2[k])), 1e-13 * scale) << c.name << ", state " << s << ", input " << k;
        EXPECT_LE(std::fabs(sb1[k] - jt), 1e-12 * scale) << c.name << ", state " << s << ", input " << k << ": adjoint " << sb1[k] << " vs J^T seed " << jt;
      }
    }
  }
}
