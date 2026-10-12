// PRINCIPLES.md §1b invariant I2, E0 gate: making solver::implicit_dual nest must not disturb the
// first-order answer, and must not change the elimination order it is computed by.
//
// Three claims, all BITWISE, over the calibrated path (quotes -> implicit_dual -> price_book) and
// over the two-block chain:
//
//   (1) the nested pass's value channel  r.v.v      is the plain `double` pass;
//   (2) its outer tangent channel        r.d[i].v   is the plain Dual<N> pass's slot i;
//   (3) its inner tangent channel        r.v.d[i]   is the same Dual<N> pass's slot i.
//
// §5.2a case 1: all three sides are the same expressions evaluated by the same sequence of IEEE
// operations on the same inputs — the Scalar differs, the arithmetic does not. The Newton
// iteration itself is reached exactly once however deep the nesting goes (solver/tangent.hpp), so
// the value channel is literally the same solve. A failure here is a defect in the scalar or in
// the rule, never a reason to loosen the gate.
//
// And (4), the one that makes (1)–(3) possible at all: the LU's PIVOT SEQUENCE. Partial pivoting
// on a Dual decides on the VALUE channel, so the elimination order is the plain-`double`
// factorisation's, at every nesting depth; and the factorisation's own value channel is bitwise
// the double factorisation. A pivot allowed to see a tangent would be a different elimination of
// the same matrix, and (2) and (3) would be false.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention), which
// is what makes (1)–(4) theorems: under contraction the compiler contracts the three
// instantiations differently and they are NOT bitwise (measured on this fixture at
// -O3 -march=x86-64-v3 with contraction allowed: the two gradient channels of a 1,000-swap book
// drift up to 2.9e-6 absolute on a book delta of 5.5e+9, ~5e-16 relative). That is the whole
// reason the E0 gates live in their own TUs (D8, D25).
//
// A separate, stronger check was run by hand when the nesting landed and cannot be expressed as a
// test: the same dump of the first-order IFT answer (the 12 states' tangents, 7 swap PVs' and the
// book PV's tangents, and an FNV hash over all 12,012 book tangents, plus the two-curve chain's
// 5 trades x 18) compiled against the PRE-nesting tangent.hpp and against this one is byte-for-byte
// identical — under -ffp-contract=off AND under the release flags.
//
// The second derivative itself has no bitwise reference — nothing else in the engine computes one
// — so it is gated against central differences in tests/solver/second_order_ift_test.cpp.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_calibration.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/fixtures/second_order_ift.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/solver/tangent.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace fx = epykos::fixtures;
namespace solver = epykos::solver;
using epykos::Dual;

namespace {

constexpr int Nq = fx::so_m1_quotes;  // 12
constexpr int Nt = fx::so_2c_quotes;  // 18

std::uint64_t bits(double x) {
  std::uint64_t b = 0;
  std::memcpy(&b, &x, sizeof b);
  return b;
}

const fx::Book& book() {
  static const fx::Book b = fx::make_m1_book();
  return b;
}
const std::vector<fx::CalSwap>& disc_swaps() {
  static const std::vector<fx::CalSwap> s = fx::m1_calibration_swaps();
  return s;
}
const std::vector<fx::CalSwap>& proj_swaps() {
  static const std::vector<fx::CalSwap> s = fx::proj_calibration_swaps();
  return s;
}
const std::vector<double>& m1_quotes() {
  static const std::vector<double> q = fx::m1_par_quotes(book().z0.data());
  return q;
}
const std::vector<double>& two_curve_quotes() {
  static const std::vector<double> q = fx::two_curve_quotes(fx::record_state.data(), fx::proj_record_state.data());
  return q;
}

struct M1 {
  template <class Scalar>
  void operator()(const Scalar* q, Scalar* out) const {
    fx::calibrated_m1<Scalar>(book(), disc_swaps(), q, out);
  }
};
struct TwoCurve {
  template <class Scalar>
  void operator()(const Scalar* q, Scalar* out) const {
    fx::two_curve_chain<Scalar>(disc_swaps(), proj_swaps(), two_curve_quotes().data(), q, out);
  }
};

// (1)-(3) for one fixture at one point.
template <int N, class F>
void check_first_order(const char* what, F&& f, const double* q, int n_out) {
  const fx::NestedRun<N> nested = fx::nested_run<N>(f, q, n_out);

  // (1) the value channel against the plain double pass, computed in this contraction-free TU.
  std::vector<double> dv(static_cast<std::size_t>(n_out));
  f(q, dv.data());
  std::size_t bad_v = 0;
  for (int o = 0; o < n_out; ++o) {
    const std::size_t so = static_cast<std::size_t>(o);
    if (bits(nested.value[so]) != bits(dv[so])) {
      if (bad_v < 3) {
        ADD_FAILURE() << what << ": value channel of output " << o << " is " << nested.value[so] << ", the double pass "
                      << "gives " << dv[so];
      }
      ++bad_v;
    }
  }
  EXPECT_EQ(bad_v, 0u) << what << ": " << bad_v << " of " << n_out << " value channels are not bitwise the double pass";

  // (2) and (3) against the plain Dual<N> pass through the same calibration.
  const std::vector<double> g = fx::gradient_exact<N>(f, q, n_out);
  std::size_t bad_outer = 0, bad_inner = 0;
  for (int o = 0; o < n_out; ++o) {
    for (int i = 0; i < N; ++i) {
      const double want = g[static_cast<std::size_t>(o) * N + static_cast<std::size_t>(i)];
      if (bits(nested.g(o, i)) != bits(want)) {
        if (bad_outer < 3) {
          ADD_FAILURE() << what << ": outer tangent [" << o << "][" << i << "] is " << nested.g(o, i) << ", Dual<" << N
                        << "> gives " << want;
        }
        ++bad_outer;
      }
      if (bits(nested.gi(o, i)) != bits(want)) {
        if (bad_inner < 3) {
          ADD_FAILURE() << what << ": inner tangent [" << o << "][" << i << "] is " << nested.gi(o, i) << ", Dual<" << N
                        << "> gives " << want;
        }
        ++bad_inner;
      }
    }
  }
  EXPECT_EQ(bad_outer, 0u) << what << ": " << bad_outer << " outer tangent slots differ from the Dual<" << N << "> pass";
  EXPECT_EQ(bad_inner, 0u) << what << ": " << bad_inner << " inner tangent slots differ from the Dual<" << N << "> pass";
}

}  // namespace

TEST(SecondOrderIftE0, NestedPassKeepsTheM1FirstOrderAnswer) {
  check_first_order<Nq>("M1 calibration, par quotes", M1{}, m1_quotes().data(), fx::so_m1_outputs());
}

// Off the record point too: the solve takes a different path, so the claim is about the rule and
// not about one lucky iterate. Both quote sets are shifted bodily, which keeps the curve sane.
TEST(SecondOrderIftE0, NestedPassKeepsTheM1FirstOrderAnswerAwayFromTheRecordPoint) {
  for (double bump : {-0.0075, 0.0125}) {
    std::vector<double> q = m1_quotes();
    for (std::size_t k = 0; k < q.size(); ++k) q[k] += bump * (1.0 + 0.1 * static_cast<double>(k));
    check_first_order<Nq>(("M1 calibration, quotes " + std::to_string(bump)).c_str(), M1{}, q.data(),
                          fx::so_m1_outputs());
  }
}

TEST(SecondOrderIftE0, NestedPassKeepsTheTwoCurveChainFirstOrderAnswer) {
  check_first_order<Nt>("two-curve chain", TwoCurve{}, two_curve_quotes().data(), fx::so_2c_outputs);
}

// ---- (4) the pivot -----------------------------------------------------------------------------

TEST(SecondOrderIftE0, ThePivotSequenceIsTheDoubleFactorisationsAtEveryDepth) {
  using Inner = Dual<Nq>;
  using Outer = Dual<Nq, Inner>;
  const std::vector<double>& q = m1_quotes();
  auto R = [&](const auto* z, const auto* p, auto* F) { fx::m1_residual(disc_swaps(), z, p, F); };

  // The same solution, reached three times, and F_z built over three scalars at it.
  std::vector<double> qd0(q.begin(), q.end()), z0(Nq, fx::so_ift_start);
  const solver::SolveReport rep = solver::solve_newton(R, Nq, Nq, Nq, qd0.data(), z0.data(), fx::so_ift_options());
  ASSERT_TRUE(rep.converged);

  std::vector<double> F0(Nq), J0(static_cast<std::size_t>(Nq) * Nq);
  solver::jacobian_dual(R, Nq, Nq, Nq, qd0.data(), z0.data(), F0.data(), J0.data());

  std::vector<Inner> q1(Nq), z1(Nq);
  for (int k = 0; k < Nq; ++k) {
    q1[static_cast<std::size_t>(k)] = Inner::variable(q[static_cast<std::size_t>(k)], k);
    z1[static_cast<std::size_t>(k)] = Inner(z0[static_cast<std::size_t>(k)]);
  }
  std::vector<Inner> F1(Nq), J1(static_cast<std::size_t>(Nq) * Nq);
  solver::jacobian_dual(R, Nq, Nq, Nq, q1.data(), z1.data(), F1.data(), J1.data());

  std::vector<Outer> q2(Nq), z2(Nq);
  for (int k = 0; k < Nq; ++k) {
    q2[static_cast<std::size_t>(k)] = Outer::variable(Inner::variable(q[static_cast<std::size_t>(k)], k), k);
    z2[static_cast<std::size_t>(k)] = Outer(Inner(z0[static_cast<std::size_t>(k)]));
  }
  std::vector<Outer> F2(Nq), J2(static_cast<std::size_t>(Nq) * Nq);
  solver::jacobian_dual(R, Nq, Nq, Nq, q2.data(), z2.data(), F2.data(), J2.data());

  // F_z's own value channel is the same matrix at all three depths, bitwise.
  for (std::size_t t = 0; t < J0.size(); ++t) {
    ASSERT_EQ(bits(J1[t].v), bits(J0[t])) << "F_z[" << t << "] value channel differs at depth 1";
    ASSERT_EQ(bits(J2[t].v.v), bits(J0[t])) << "F_z[" << t << "] value channel differs at depth 2";
  }

  std::vector<int> p0, p1, p2;
  solver::detail::lu_factor(Nq, J0, p0);
  solver::detail::lu_factor(Nq, J1, p1);
  solver::detail::lu_factor(Nq, J2, p2);
  EXPECT_EQ(p1, p0) << "the depth-1 factorisation picked a different pivot sequence from the double one";
  EXPECT_EQ(p2, p0) << "the depth-2 factorisation picked a different pivot sequence from the double one";
  for (std::size_t t = 0; t < J0.size(); ++t) {
    EXPECT_EQ(bits(J1[t].v), bits(J0[t])) << "LU[" << t << "] value channel differs at depth 1";
    EXPECT_EQ(bits(J2[t].v.v), bits(J0[t])) << "LU[" << t << "] value channel differs at depth 2";
  }

  // And the triangular solve: same permutation, same value channel.
  std::vector<double> b0(Nq), x0(Nq);
  std::vector<Inner> b1(Nq), x1(Nq);
  std::vector<Outer> b2(Nq), x2(Nq);
  for (int i = 0; i < Nq; ++i) {
    const double v = 1.0 / (2.0 + static_cast<double>(i));
    b0[static_cast<std::size_t>(i)] = v;
    b1[static_cast<std::size_t>(i)] = Inner::variable(v, i % Nq);
    b2[static_cast<std::size_t>(i)] = Outer::variable(Inner::variable(v, i % Nq), i % Nq);
  }
  solver::detail::lu_solve(Nq, J0, p0, b0.data(), x0.data());
  solver::detail::lu_solve(Nq, J1, p1, b1.data(), x1.data());
  solver::detail::lu_solve(Nq, J2, p2, b2.data(), x2.data());
  for (int i = 0; i < Nq; ++i) {
    const std::size_t si = static_cast<std::size_t>(i);
    EXPECT_EQ(bits(x1[si].v), bits(x0[si])) << "lu_solve[" << i << "] value channel differs at depth 1";
    EXPECT_EQ(bits(x2[si].v.v), bits(x0[si])) << "lu_solve[" << i << "] value channel differs at depth 2";
  }
}

// A singular Jacobian must still be detected on the value channel, at every depth: the pivot test
// is `best == 0.0` on a value, and a tangent must not rescue a zero pivot.
TEST(SecondOrderIftE0, ASingularMatrixThrowsAtEveryDepth) {
  using Inner = Dual<2>;
  using Outer = Dual<2, Inner>;
  std::vector<double> a0 = {1.0, 2.0, 2.0, 4.0};  // rank 1
  std::vector<Inner> a1(4);
  std::vector<Outer> a2(4);
  for (std::size_t t = 0; t < 4; ++t) {
    a1[t] = Inner::variable(a0[t], 0);  // every entry carries a nonzero tangent
    a2[t] = Outer::variable(Inner::variable(a0[t], 0), 1);
  }
  std::vector<int> perm;
  EXPECT_THROW(solver::detail::lu_factor(2, a0, perm), std::runtime_error);
  EXPECT_THROW(solver::detail::lu_factor(2, a1, perm), std::runtime_error);
  EXPECT_THROW(solver::detail::lu_factor(2, a2, perm), std::runtime_error);
}
